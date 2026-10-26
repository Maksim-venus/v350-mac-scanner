/*
 * v350lib.c — Epson Perfection V350 (04b8:012f) host via Interpreter 68.bundle
 * Protocol notes: docs/PROTOCOL.md.
 *
 * Safety rules (after the 2026-10-26 1200-dpi belt-grind incident):
 *  - never send ESC e / ESC f while an ESC G image transfer is in progress;
 *  - abort = CAN in place of the next block ACK, drain the reply, then release;
 *  - on a USB stall/timeout mid-transfer, send nothing further (wedged) and
 *    tell the user to power-cycle;
 *  - no libusb_reset_device anywhere.
 */
#include "v350lib.h"
#include <CoreFoundation/CoreFoundation.h>
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* Interpreter 68.bundle (with Esfw68.bin in its Resources) comes from Epson's official ICA
   scanner driver (ESICA 5.8.23). It is NOT shipped with this project. */
#define BUNDLE_PATH \
  "/Library/Image Capture/Devices/EPSON Scanner.app/Contents/PlugIns/Interpreter 68.bundle"
#define BUNDLE_PATH_USER \
  "Library/Image Capture/Devices/EPSON Scanner.app/Contents/PlugIns/Interpreter 68.bundle"
static const char *g_interp_override = NULL;
void v350_set_interpreter(const char *path) { g_interp_override = path && *path ? path : NULL; }
static int is_dir(const char *p) { struct stat st; return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
const char *v350_find_interpreter(void) {
  static char buf[1200];
  const char *o = g_interp_override ? g_interp_override : getenv("V350_INTERPRETER");
  if (o && *o) {                     /* a bundle, or EPSON Scanner.app, or the folder containing it */
    const char *cand[] = {"%s", "%s/Contents/PlugIns/Interpreter 68.bundle",
                          "%s/EPSON Scanner.app/Contents/PlugIns/Interpreter 68.bundle"};
    for (int i = 0; i < 3; i++) { snprintf(buf, sizeof buf, cand[i], o);
      char exe[1300]; snprintf(exe, sizeof exe, "%s/Contents/MacOS/Interpreter 68", buf);
      if (access(exe, R_OK) == 0) return buf; }
    return NULL;
  }
  if (is_dir(BUNDLE_PATH)) return BUNDLE_PATH;
  const char *home = getenv("HOME");
  if (home) { snprintf(buf, sizeof buf, "%s/%s", home, BUNDLE_PATH_USER); if (is_dir(buf)) return buf; }
  return NULL;
}
int v350_firmware_present(const char *bundle) {
  char fw[1300]; if (!bundle) return 0;
  snprintf(fw, sizeof fw, "%s/Contents/Resources/Esfw68.bin", bundle);
  return access(fw, R_OK) == 0;
}
#define ESC 0x1b
#define ACK 0x06
#define NAK 0x15
#define STX 0x02
#define CAN 0x18

/* Timeouts (ms) */
#define TO_CMD        30000   /* ordinary command replies */
#define TO_FIRST_LINE 120000  /* ESC G: lamp warm-up + calibration before first line */
#define TO_STALL      20000   /* ESC G: max gap between lines once streaming */
#define TO_STALL_HI   60000   /* same, for 2400/4800 dpi (slow carriage, buffer backtracks) */

typedef bool (*pmdd_io)(uint8_t *buf, uint32_t len, void *ctx, int16_t *err);
typedef bool (*fn_init)(pmdd_io, pmdd_io, void *);
typedef bool (*fn_rw)(uint8_t *, uint32_t);
typedef void (*fn_close)(void);
typedef int16_t (*fn_usberr)(void);
typedef uint32_t (*fn_interr)(void);

static struct {
  libusb_context *ctx;
  libusb_device_handle *h;
  int iface, claimed;
  uint8_t ep_in, ep_out;
  CFBundleRef bundle;
  fn_init INTInit;
  fn_rw INTRead, INTWrite;
  fn_close INTClose;
  fn_usberr INTGetUSBError;
  fn_interr INTGetInterpreterError;
  int verbose;
  int opened, int_inited;
  int speed;
  volatile int wedged;          /* USB-level failure: stop talking to device */
  unsigned timeout_ms;          /* current bulk timeout */
  unsigned stall_ms;
  int last_usb_err;
  /* stats */
  uint64_t rd_bytes, rd_calls;
  double rd_time;
} G;

static volatile sig_atomic_t g_abort = 0;
void v350_request_abort(void) { g_abort = 1; }
int v350_is_wedged(void) { return G.wedged; }
int v350_usb_speed(void) { return G.speed; }

static double now_s(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static void seterr(v350_t *s, const char *fmt, ...) {
  if (!s) return;
  va_list ap; va_start(ap, fmt);
  vsnprintf(s->err, sizeof s->err, fmt, ap);
  va_end(ap);
  if (G.verbose) fprintf(stderr, "v350: %s\n", s->err);
}

static void hexdump(const char *tag, const uint8_t *b, size_t n) {
  fprintf(stderr, "%s[%zu]:", tag, n);
  for (size_t i = 0; i < n && i < 48; i++) fprintf(stderr, " %02x", b[i]);
  if (n > 48) fprintf(stderr, " ...");
  fprintf(stderr, "\n");
}

/* USB callbacks used by the interpreter (it may call these from its own
   read-ahead thread). One libusb transfer per request; a timeout with zero
   bytes marks the device wedged so the host stops issuing commands. */
static bool cb_read(uint8_t *buf, uint32_t len, void *ctx, int16_t *err) {
  (void)ctx;
  if (G.wedged) { if (err) *err = -1; return false; }
  double t0 = now_s();
  uint32_t got = 0;
  while (got < len) {
    int n = 0;
    int r = libusb_bulk_transfer(G.h, G.ep_in, buf + got, (int)(len - got), &n,
                                 G.timeout_ms ? G.timeout_ms : TO_CMD);
    got += (uint32_t)(n > 0 ? n : 0);
    if (r == LIBUSB_ERROR_TIMEOUT && n > 0) continue;
    if (r) {
      G.last_usb_err = r;
      G.wedged = 1;
      fprintf(stderr, "v350: USB read %s after %u/%u bytes (timeout %u ms) -> WEDGED\n",
              libusb_error_name(r), got, len, G.timeout_ms);
      if (err) *err = (int16_t)r;
      return false;
    }
  }
  G.rd_time += now_s() - t0; G.rd_bytes += len; G.rd_calls++;
  return true;
}
static bool cb_write(uint8_t *buf, uint32_t len, void *ctx, int16_t *err) {
  (void)ctx;
  if (G.wedged) { if (err) *err = -1; return false; }
  uint32_t put = 0;
  while (put < len) {
    int n = 0;
    int r = libusb_bulk_transfer(G.h, G.ep_out, buf + put, (int)(len - put), &n, TO_CMD);
    if (r) {
      G.last_usb_err = r; G.wedged = 1;
      fprintf(stderr, "v350: USB write %s -> WEDGED\n", libusb_error_name(r));
      if (err) *err = (int16_t)r;
      return false;
    }
    put += (uint32_t)n;
  }
  return true;
}

static void *sym(CFBundleRef b, const char *name) {
  CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
  void *p = CFBundleGetFunctionPointerForName(b, s);
  CFRelease(s);
  return p;
}

static bool iw(const uint8_t *b, uint32_t n) {
  if (G.wedged) return false;
  if (G.verbose > 2) hexdump("W", b, n);
  return G.INTWrite((uint8_t *)b, n);
}
static bool ir(uint8_t *b, uint32_t n) {
  if (G.wedged) return false;
  bool ok = G.INTRead(b, n);
  if (G.verbose > 2 && ok) hexdump("R", b, n);
  return ok;
}

/* ESC set: ESC x -> ACK, params -> ACK/NAK */
static int esc_set(v350_t *s, uint8_t cmd, const uint8_t *p, uint32_t n) {
  uint8_t c[2] = { ESC, cmd }, a = 0;
  if (!iw(c, 2) || !ir(&a, 1)) { seterr(s, "ESC %c cmd io fail", cmd); return -1; }
  if (a != ACK) { seterr(s, "ESC %c cmd NAK/0x%02x", cmd, a); return -1; }
  if (!iw(p, n) || !ir(&a, 1)) { seterr(s, "ESC %c param io fail", cmd); return -1; }
  if (G.verbose > 1) fprintf(stderr, "ESC %c ok->0x%02x\n", cmd, a);
  if (a != ACK) { seterr(s, "ESC %c param NAK", cmd); return 1; }
  return 0;
}

static int esc_cmd_ack(v350_t *s, uint8_t cmd) {
  uint8_t c[2] = { ESC, cmd }, a = 0;
  if (!iw(c, 2) || !ir(&a, 1)) { seterr(s, "ESC %c io fail", cmd); return -1; }
  if (G.verbose) fprintf(stderr, "ESC %c -> 0x%02x\n", cmd, a);
  return a == ACK ? 0 : -1;
}

/* ESC request: ESC x -> 4-byte header then payload */
static int esc_req(v350_t *s, uint8_t cmd, uint8_t *out, uint32_t outmax, uint8_t *st) {
  uint8_t c[2] = { ESC, cmd }, hdr[4];
  if (!iw(c, 2) || !ir(hdr, 4)) { seterr(s, "ESC %c header fail", cmd); return -1; }
  if (st) *st = hdr[1];
  uint32_t len = hdr[2] | (hdr[3] << 8);
  if (G.verbose) fprintf(stderr, "ESC %c hdr %02x %02x len=%u\n", cmd, hdr[0], hdr[1], len);
  if (hdr[0] != STX || len > outmax) { seterr(s, "ESC %c bad header", cmd); return -1; }
  if (len && !ir(out, len)) { seterr(s, "ESC %c data fail", cmd); return -1; }
  return (int)len;
}

static const char *speed_name(int sp) {
  switch (sp) {
    case LIBUSB_SPEED_LOW: return "Low (1.5 Mb/s)";
    case LIBUSB_SPEED_FULL: return "Full (12 Mb/s)";
    case LIBUSB_SPEED_HIGH: return "High (480 Mb/s)";
    case LIBUSB_SPEED_SUPER: return "Super (5 Gb/s)";
    default: return "unknown";
  }
}

int v350_open(v350_t *s) {
  int need_hs = s->require_high_speed, verbose = s->verbose;
  memset(s, 0, sizeof *s);
  s->require_high_speed = need_hs; s->verbose = verbose;
  G.verbose = verbose;
  if (G.opened) return 0;
  G.wedged = 0; G.timeout_ms = TO_CMD;

  /* --- USB first: speed check happens BEFORE the interpreter sends anything --- */
  if (libusb_init(&G.ctx)) { seterr(s, "libusb_init failed"); return -1; }
  G.h = libusb_open_device_with_vid_pid(G.ctx, V350_VID, V350_PID);
  if (!G.h) { seterr(s, "scanner 04b8:012f not found (unplugged, or Image Capture holds it?)"); v350_close(s); return -1; }
  libusb_device *dev = libusb_get_device(G.h);
  G.speed = libusb_get_device_speed(dev);
  if (verbose) fprintf(stderr, "USB link: %s\n", speed_name(G.speed));
  if (need_hs && G.speed < LIBUSB_SPEED_HIGH) {
    seterr(s, "USB link is %s, not High-Speed. Refusing >=1200 dpi. "
              "Unplug the scanner USB cable, power-cycle the scanner, plug it directly "
              "into the Mac (no hub), then retry.", speed_name(G.speed));
    v350_close(s); return -1;
  }

  int cur = 0;
  if (libusb_get_configuration(G.h, &cur) == 0 && cur == 0) {
    if (libusb_set_configuration(G.h, 1) != 0) { seterr(s, "set_configuration(1) failed"); v350_close(s); return -1; }
  }
  struct libusb_config_descriptor *cfg = NULL;
  if (libusb_get_active_config_descriptor(dev, &cfg) && libusb_get_config_descriptor(dev, 0, &cfg)) {
    seterr(s, "get config descriptor failed"); v350_close(s); return -1;
  }
  const struct libusb_interface_descriptor *id = &cfg->interface[0].altsetting[0];
  G.iface = id->bInterfaceNumber;
  for (int i = 0; i < id->bNumEndpoints; i++) {
    const struct libusb_endpoint_descriptor *ep = &id->endpoint[i];
    if ((ep->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK) {
      if (ep->bEndpointAddress & 0x80) { if (!G.ep_in) G.ep_in = ep->bEndpointAddress; }
      else if (!G.ep_out) G.ep_out = ep->bEndpointAddress;
    }
  }
  libusb_free_config_descriptor(cfg);
  if (!G.ep_in || !G.ep_out) { seterr(s, "no bulk endpoints"); v350_close(s); return -1; }
  int r = libusb_claim_interface(G.h, G.iface);
  if (r) { seterr(s, "claim_interface: %s (quit Image Capture)", libusb_error_name(r)); v350_close(s); return -1; }
  G.claimed = 1;

  /* --- interpreter --- */
  if (!G.bundle) {
    const char *bp = v350_find_interpreter();
    if (!bp) { seterr(s, "DRIVER_MISSING: Interpreter 68.bundle not found - install Epson ICA Scanner Driver 5.8.23"); v350_close(s); return -1; }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)bp, (CFIndex)strlen(bp), true);
    G.bundle = CFBundleCreate(NULL, url);
    CFRelease(url);
    if (!G.bundle) { seterr(s, "DRIVER_MISSING: cannot open Interpreter 68.bundle (install Epson ICA Scanner Driver 5.8.23)"); v350_close(s); return -1; }
    CFErrorRef e = NULL;
    if (!CFBundleLoadExecutableAndReturnError(G.bundle, &e)) {
      if (e) CFRelease(e);
      seterr(s, "cannot load interpreter executable"); v350_close(s); return -1;
    }
    G.INTInit = (fn_init)sym(G.bundle, "INTInit");
    G.INTRead = (fn_rw)sym(G.bundle, "INTRead");
    G.INTWrite = (fn_rw)sym(G.bundle, "INTWrite");
    G.INTClose = (fn_close)sym(G.bundle, "INTClose");
    G.INTGetUSBError = (fn_usberr)sym(G.bundle, "INTGetUSBError");
    G.INTGetInterpreterError = (fn_interr)sym(G.bundle, "INTGetInterpreterError");
    if (!G.INTInit || !G.INTRead || !G.INTWrite || !G.INTClose) { seterr(s, "missing INT* symbols"); v350_close(s); return -1; }
  }
  if (verbose) fprintf(stderr, "INTInit...\n");
  if (!G.INTInit(cb_read, cb_write, NULL)) {
    seterr(s, "INTInit failed (usb=%d interp=%u)",
           G.INTGetUSBError ? G.INTGetUSBError() : -1,
           G.INTGetInterpreterError ? G.INTGetInterpreterError() : 0);
    v350_close(s); return -1;
  }
  G.int_inited = 1;
  G.opened = 1;
  /* ESC @: reset; the interpreter waits until the scanner is ready before answering */
  if (esc_cmd_ack(s, '@') != 0) {
    if (G.wedged) { v350_close(s); return -1; }
    if (verbose) fprintf(stderr, "warning: ESC @ not ACK\n");
  }
  return 0;
}

/* Sends NO ESC/I commands: release/eject are explicit, separate calls. */
void v350_close(v350_t *s) {
  (void)s;
  if (G.int_inited && G.INTClose) G.INTClose();
  G.int_inited = 0;
  if (G.h) {
    if (G.claimed) libusb_release_interface(G.h, G.iface);
    libusb_close(G.h);
    G.h = NULL;
  }
  G.claimed = 0;
  if (G.ctx) { libusb_exit(G.ctx); G.ctx = NULL; }
  G.opened = 0;
  G.ep_in = G.ep_out = 0;
}

int v350_status(v350_t *s, v350_status_t *st) {
  memset(st, 0, sizeof *st);
  uint8_t b[64], hs = 0;
  /* ESC F reply: STX, status, 0, 0 */
  if (esc_req(s, 'F', b, sizeof b, &hs) < 0) return -1;
  if (hs & 0x80) st->fatal = 1;
  int n = esc_req(s, 'f', b, sizeof b, &hs);
  if (n < 11) { if (n >= 0) seterr(s, "ESC f short reply (%d)", n); return -1; }
  if (b[0] & 0x80) st->fatal = 1;
  if (b[0] & 0x02) st->warming = 1;
  st->afl_installed = (b[6] & 0x80) != 0;
  st->afl_enabled = (b[6] & 0x40) != 0;
  st->area_x = b[7] | (b[8] << 8);
  st->area_y = b[9] | (b[10] << 8);
  if (G.verbose)
    fprintf(stderr, "status: ESC F=0x%02x ESC f[0]=0x%02x opt=0x%02x area=%ux%u (@4800 = %.2f\"x%.2f\")\n",
            hs, b[0], b[6], st->area_x, st->area_y, st->area_x / 4800.0, st->area_y / 4800.0);
  return 0;
}

int v350_wait_ready(v350_t *s, int timeout_s) {
  double t0 = now_s();
  for (;;) {
    v350_status_t st;
    if (v350_status(s, &st) != 0) return -1;
    if (st.fatal) { seterr(s, "scanner reports FATAL error status; power-cycle it"); return -1; }
    if (!st.warming) return 0;
    if (now_s() - t0 > timeout_s) { seterr(s, "scanner still warming up after %d s", timeout_s); return -1; }
    if (G.verbose) fprintf(stderr, "warming up...\n");
    sleep(1);
  }
}

int v350_set_source(v350_t *s, v350_source_t src) {
  uint8_t v = (uint8_t)src;
  int r = esc_set(s, 'e', &v, 1);
  if (r != 0) return -1;
  if (src == V350_SRC_AFL) {
    v350_status_t st;
    if (v350_status(s, &st) != 0) return -1;
    if (!st.afl_enabled) { seterr(s, "AFL not enabled after ESC e 5"); return -1; }
  }
  return 0;
}

int v350_release(v350_t *s) {
  if (G.wedged) { seterr(s, "device wedged; not sending release"); return -1; }
  uint8_t z = 0;
  return esc_set(s, 'e', &z, 1) == 0 ? 0 : -1;
}

/* Experimental eject: ESC w, then ESC e 0. On the test unit this did NOT move the strip and once
   left the scanner unresponsive, so the CLI hides it; use the scanner's eject button instead. */
int v350_eject(v350_t *s) {
  if (G.wedged) { seterr(s, "device wedged; not sending eject"); return -1; }
  /* Right after a scan the scanner is still parking; wait until it reports ready. */
  if (v350_wait_ready(s, 120) != 0) return -1;
  if (esc_cmd_ack(s, 'w') != 0) { if (!s->err[0]) seterr(s, "ESC w not ACKed"); return -1; }
  return v350_release(s);
}

void v350_image_free(v350_image_t *im) {
  if (!im) return;
  free(im->data);
  memset(im, 0, sizeof *im);
}

/* Cancel: send CAN in place of the next block ACK; the scanner stops the transfer and
   answers with one more byte, which we drain. */
static int cancel_at_block(v350_t *s) {
  uint8_t can = CAN, r = 0;
  fprintf(stderr, "v350: sending CAN at block boundary...\n");
  G.timeout_ms = TO_CMD;
  if (!iw(&can, 1)) return -1;
  for (int i = 0; i < 2; i++) {
    if (!ir(&r, 1)) return -1;
    if (G.verbose) fprintf(stderr, "v350: cancel reply 0x%02x\n", r);
    if (r == ACK) break;
  }
  (void)s;
  return 0;
}

static v350_rc_t esc_g_read(v350_t *s, int bits, int channels, int exp_w, int exp_h,
                            v350_line_cb cb, void *ctx, int *out_w, int *out_h) {
  uint8_t cmd[2] = { ESC, 'G' };
  size_t bpp = (size_t)(bits / 8) * channels;
  size_t maxbpl = (size_t)exp_w * bpp;
  uint8_t *line = malloc(maxbpl * 4 + 65536);
  if (!line) { seterr(s, "oom"); return V350_ERR_CLEAN; }
  size_t total = 0;
  int width = 0, height = 0;
  double t0 = now_s(), tlast = t0;
  G.rd_bytes = G.rd_calls = 0; G.rd_time = 0;

  G.timeout_ms = TO_FIRST_LINE;
  if (!iw(cmd, 2)) { free(line); seterr(s, "ESC G write failed"); return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN; }

  for (;;) {
    uint8_t hdr[6] = {0};
    if (!ir(hdr, 6)) {
      free(line);
      seterr(s, "ESC G header read failed at line %d (%s)", height,
             G.wedged ? libusb_error_name(G.last_usb_err) : "interpreter");
      return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
    }
    if (hdr[0] != STX) {
      seterr(s, "ESC G bad STX 0x%02x at line %d", hdr[0], height);
      free(line);
      return cancel_at_block(s) == 0 ? V350_ERR_CLEAN : V350_ERR_WEDGED;
    }
    uint8_t st = hdr[1];
    uint32_t bpl = hdr[2] | (hdr[3] << 8);
    uint32_t lines = hdr[4] | (hdr[5] << 8);
    if (st & 0x80) {
      seterr(s, "ESC G fatal status 0x%02x at line %d", st, height);
      uint8_t d = 0; ir(&d, 1);
      free(line); return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
    }
    if (bpl == 0) {
      uint8_t d = 0; ir(&d, 1);
      if (st & 0x20) break;
      seterr(s, "ESC G bpl=0 status=0x%02x", st);
      free(line); return V350_ERR_CLEAN;
    }
    if (lines == 0) lines = 1;                 /* USB block mode: one line per block */
    size_t chunk = (size_t)bpl * lines;
    if (bpl > maxbpl || chunk > maxbpl * 4 + 65536 || height + (int)lines > exp_h) {
      seterr(s, "unexpected block geometry bpl=%u lines=%u at line %d (window %dx%d)",
             bpl, lines, height, exp_w, exp_h);
      free(line);
      return cancel_at_block(s) == 0 ? V350_ERR_CLEAN : V350_ERR_WEDGED;
    }
    if (!ir(line, (uint32_t)chunk)) {
      free(line);
      seterr(s, "ESC G data read failed at line %d (%s)", height,
             G.wedged ? libusb_error_name(G.last_usb_err) : "interpreter");
      return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
    }
    if (!width) width = (int)(bpl / bpp);
    for (uint32_t k = 0; k < lines; k++) {
      if (cb(ctx, line + (size_t)k * bpl, bpl, height + (int)k) != 0) {
        seterr(s, "output write failed at line %d", height);
        free(line);
        return cancel_at_block(s) == 0 ? V350_ERR_CLEAN : V350_ERR_WEDGED;
      }
    }
    total += chunk;
    height += (int)lines;
    G.timeout_ms = G.stall_ms;                 /* streaming: stall watchdog */
    double tn = now_s();
    if (G.verbose && (height % 1000 == 0 || tn - tlast > 10)) {
      double el = tn - t0;
      fprintf(stderr, "  %d/%d lines  %.0f s  %.0f KB/s\n", height, exp_h, el,
              total / 1024.0 / (el > 0 ? el : 1));
      tlast = tn;
    }
    if (st & 0x20) break;                      /* AREA_END */
    if (g_abort) {
      seterr(s, "aborted by user at line %d", height);
      free(line);
      return cancel_at_block(s) == 0 ? V350_ERR_CLEAN : V350_ERR_WEDGED;
    }
    uint8_t ack = ACK;
    if (!iw(&ack, 1)) { free(line); seterr(s, "ESC G ACK write failed"); return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN; }
  }
  free(line);
  G.timeout_ms = TO_CMD;
  double el = now_s() - t0;
  *out_w = width; *out_h = height;
  fprintf(stderr, "segment data: %dx%d %d-bit, %zu bytes in %.1f s (%.0f KB/s)\n",
          width, height, bits, total, el, total / 1024.0 / (el > 0 ? el : 1));
  return V350_OK;
}

/* Fresh session state for one window: ESC @, ESC e 5, ready. Mirrors the
   hardware-tested open sequence, so every segment starts from the same state. */
int v350_prepare_afl(v350_t *s) {
  if (G.wedged) { seterr(s, "device wedged"); return -1; }
  if (v350_wait_ready(s, 120) != 0) return -1;
  if (esc_cmd_ack(s, '@') != 0) { if (G.wedged) return -1; if (G.verbose) fprintf(stderr, "warning: ESC @ not ACK\n"); }
  if (v350_set_source(s, V350_SRC_AFL) != 0) return -1;
  return v350_wait_ready(s, 120);
}

v350_rc_t v350_scan_window(v350_t *s, int dpi, int bits, v350_color_t color, v350_film_t film,
                           int y_px, int h_px, v350_line_cb cb, void *ctx, int *out_w, int *out_h) {
  *out_w = *out_h = 0;
  if (G.wedged) { seterr(s, "device wedged"); return V350_ERR_WEDGED; }
  if (!(dpi == 300 || dpi == 600 || dpi == 1200 || dpi == 2400 || dpi == 4800)) {
    seterr(s, "AFL dpi must be 300/600/1200/2400/4800"); return V350_ERR_CLEAN;
  }
  if (bits != 8 && bits != 16) { seterr(s, "bits must be 8 or 16"); return V350_ERR_CLEAN; }
  int channels = (color == V350_COLOR_GRAY) ? 1 : 3;

  G.stall_ms = dpi >= 2400 ? TO_STALL_HI : TO_STALL;
  /* ESC A units = pixels at the ESC R resolution (area limit accepted by the scanner:
     y+h <= 9.30*dpi + 32*dpi/2400). Width = full 1.45", multiple of 8. */
  int ymax = V350_AFL_H100 * dpi / 100;
  if (y_px < 0) y_px = 0;
  if (h_px <= 0 || y_px + h_px > ymax) { seterr(s, "window y=%d h=%d outside 0..%d", y_px, h_px, ymax); return V350_ERR_CLEAN; }
  int x = 0, y = y_px, h = h_px;
  int w = (V350_AFL_W100 * dpi / 100) / 8 * 8;
  if (h > 0xffff || y + h > 0xffff) { seterr(s, "window too tall for ESC A"); return V350_ERR_CLEAN; }
  if (bits == 16 && w > 0x2aa8) { seterr(s, "width %d exceeds 16-bit colour limit", w); return V350_ERR_CLEAN; }

  uint8_t p[8];
  p[0] = (uint8_t)dpi; p[1] = (uint8_t)(dpi >> 8); p[2] = p[0]; p[3] = p[1];
  if (esc_set(s, 'R', p, 4) != 0) return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
  p[0] = (uint8_t)color;
  if (esc_set(s, 'C', p, 1) != 0) return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
  p[0] = (uint8_t)bits;
  if (esc_set(s, 'D', p, 1) != 0) return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
  p[0] = (uint8_t)film;
  if (esc_set(s, 'N', p, 1) != 0) return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
  p[0] = x; p[1] = x >> 8; p[2] = y; p[3] = y >> 8; p[4] = w; p[5] = w >> 8; p[6] = h; p[7] = h >> 8;
  fprintf(stderr, "window: ESC A x=%d y=%d w=%d h=%d px @ %d dpi = %.3f\" x %.3f\" at y=%.2f\"\n",
          x, y, w, h, dpi, w / (double)dpi, h / (double)dpi, y / (double)dpi);
  if (esc_set(s, 'A', p, 8) != 0) {
    if (!G.wedged) { char e[256]; snprintf(e, sizeof e, "%s", s->err); seterr(s, "ESC A window rejected (%s)", e); }
    return G.wedged ? V350_ERR_WEDGED : V350_ERR_CLEAN;
  }
  return esc_g_read(s, bits, channels, w, h, cb, ctx, out_w, out_h);
}
