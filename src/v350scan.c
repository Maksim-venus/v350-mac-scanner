/*
 * v350scan — Epson Perfection V350 auto film loader: raw whole-strip scans (no crop / no invert)
 *
 *   v350scan --preview [--out P.tif]                    300 dpi strip + PNG + gap list (+ P.json)
 *   v350scan --dpi N --bits 8|16 --cuts y1,y2,... [--end Y] [--start Y] [--out P.tif]
 *   v350scan --dpi N --bits 8|16 --cuts-from P.json
 *
 * Cuts are inches from the top of the 300 dpi preview window, placed in the black
 * gaps between frames. Above 300 dpi each segment between consecutive cuts is one
 * window (segment + margin on both ends). Observed behaviour: a window's ESC A y
 * offset is realised by advancing the film with the AFL feeder; the carriage then
 * travels only the window length.
 * Windows are aligned on the 2*margin overlap and joined at the cut, snapped to
 * the darkest band (the gap).
 *
 * Calibration (strip_300_8.tif vs strip_600_seg.tif, 26 Oct 2026):
 *   - the feeder moves film ~3 % further than nominal: 300 dpi preview is compressed
 *     (frame pitch 1.445" vs 1.493" carriage-scanned; 38 mm = 1.496")  -> F_PREV 1.030
 *   - window-to-window advance ~ +21 lines per 776 nominal at 600 dpi    -> F_WIN  1.027
 *   - window y=0 starts 0.030" (true) after the preview origin           -> K      0.030
 *   true position X = preview_inches * F_PREV;  ESC A y = (X - K - bias) / F_WIN
 *   bias is re-estimated from each measured seam (adaptive).
 */
#include "v350lib.h"
#include "v350stitch.h"
#include "v350preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <strings.h>

#define MAXW        48
#define MARGIN_IN   0.12    /* each window extends this far past its cuts              */
#define WARN_LEN    1.80    /* windows longer than this print a warning                */
#define REFUSE_LEN  4.50    /* longer windows fail on the regular path (5.50" at 600)  */
#define PREVIEW_SPLIT 1.60
static int g_film = PV_CPOS;   /* --film: gap detection + display PNG only; scanning stays raw (ESC N=0) */
static int cmpdbl(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
#define PAD_IN      0.04    /* split mode: keep this much past each (snapped) cut  */
#define SNAP_IN     0.06    /* split mode: look for the gap within +- this          */  /* auto cuts: split segments longer than this (preview in)  */

static double F_PREV = 1.030, F_WIN = 1.027, K_IN = 0.030;

static void usage(const char *a) {
  fprintf(stderr,
    "Epson V350 AFL - raw strip scans (no crop, no invert, ESC N=0), uncompressed TIFF\n\n"
    "  %s --preview [--bits 8|16] [--out P.tif]\n"
    "      300 dpi whole strip (one window). Writes P.tif, P.png (auto-levelled view),\n"
    "      P.json, and prints the black gaps between frames + suggested cuts.\n"
    "  %s --dpi 600|1200|2400|4800 --bits 8|16 --cuts Y1,Y2,... [--end Y] [--start Y] [--only 2,4] [--out DIR]\n"
    "  %s --dpi N --bits B --cuts-from P.json        (cuts/end from a preview)\n"
    "      Y = inches from the top of the preview. One window per segment between cuts\n"
    "      (+%.2f\" margin each end, refused if > %.1f\"). Each segment is saved as\n"
    "      DIR/frame_NN.tif (NN = segment number from the preview top), trimmed at its\n"
    "      cuts: the whole black gap is kept (if no gap is found: cut +-0.04\", --pad IN).\n"
    "      --only N,M scans only those segments. Default DIR: ~/Pictures/V350/<YYYYMMDD_HHMMSS>/\n"
    "      --outdir DIR (default ~/Pictures/V350): save straight into DIR, numbering after the\n"
    "        highest existing frame_NN.* (selected segments only, never overwrites)\n"
    "      --out DIR: save into DIR named by segment number (frame_<N>.tif, never overwrites)\n"
    "      --stitch: old behaviour, one stitched TIFF joined at the cuts.\n"
    "  %s --dpi 300 --bits B [--out P.tif]          (same as preview, no analysis)\n\n"
    "Options: --keep-segments DIR  also save every raw window as DIR/winNN.tif\n"
    "         --progress           machine-readable PROGRESS lines on stderr\n"
    "         --live DIR           write live-view rows (DIR/live_preview.raw, live_NN.raw) for the GUI\n"
    "         --cal FPREV,FWIN,K   override calibration (default %.3f,%.3f,%.3f)\n"
    "         --dry-run            print the window plan, do not touch the scanner\n"
    "         --analyze P.tif      gap detection + PNG + JSON for an existing 300 dpi preview (no scanner)\n"
    "         --film cpos|bpos|cneg|bneg   film type for gap detection and the preview PNG\n"
    "         --finish-from P.json  negatives: save finished positives (「输出成片」) using the strip\n"
    "                      parameters computed from preview P (B&W -> greyscale TIFF). Default: raw\n"
    "         --finish     same, parameters taken from --cuts-from P.json\n"
    "         --develop RAW.tif OUT.tif --finish-from P.json [--dpi N]   finish an existing raw scan\n"
    "         --esc-n1     EXPERIMENTAL: scan negatives in the scanner's negative mode (ESC N=1),\n"
    "                      for A/B tests; use it for the preview too. Not hardware-tested\n"
    "                              (colour/B&W positive, colour/B&W negative; scans stay raw)\n"
    "         --interpreter PATH   Epson Interpreter 68.bundle (default: from the ICA driver)\n"
    "         --check-driver       report whether the Epson ICA driver is installed (no USB)\n"
    "         --status             scanner status only (no motion)\n"
    "Default output: ~/Pictures/V350/<YYYYMMDD_HHMMSS>.tif\n"
    "The strip stays in the feeder afterwards; use the scanner's eject button.\n"
    "Ctrl-C cancels cleanly at the next line. Quit Image Capture first.\n",
    a, a, a, MARGIN_IN, REFUSE_LEN, a, F_PREV, F_WIN, K_IN);
}

static void on_sigint(int sig) { (void)sig; v350_request_abort(); }

static int ensure_parent(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno != EEXIST) return -1; *p = '/'; }
  return 0;
}

static void wedged_msg(void) {
  fprintf(stderr,
    "\n*** USB transfer failed mid-scan. No further commands were sent. ***\n"
    "Power the scanner OFF, wait 10 s, power ON and let it finish initialising.\n"
    "If the strip is still in the feeder, press the scanner's eject button.\n");
}

static int cmd_status(int verbose) {
  v350_t s; memset(&s, 0, sizeof s); s.verbose = verbose ? verbose : 1;
  if (v350_open(&s) != 0) { fprintf(stderr, "open: %s\n", s.err); return 1; }
  v350_status_t st;
  int r = v350_status(&s, &st);
  if (r == 0)
    printf("fatal=%d warming=%d option_installed=%d option_enabled=%d area=%.2f\"x%.2f\" usb_speed=%d\n",
           st.fatal, st.warming, st.afl_installed, st.afl_enabled,
           st.area_x / 4800.0, st.area_y / 4800.0, v350_usb_speed());
  else fprintf(stderr, "status: %s\n", s.err);
  v350_close(&s);
  return r ? 1 : 0;
}

/* EXPERIMENTAL, hidden: ESC w + ESC e 0 did not move the strip on hardware
   and wedged once right after a scan. Kept for investigation only. */
static int cmd_eject_experimental(int verbose) {
  v350_t s; memset(&s, 0, sizeof s); s.verbose = verbose ? verbose : 1;
  if (v350_open(&s) != 0) { fprintf(stderr, "open: %s\n", s.err); return 1; }
  int r = v350_eject(&s);
  fprintf(stderr, "experimental eject: %s\n", r ? s.err : "accepted");
  if (v350_is_wedged()) wedged_msg();
  v350_close(&s);
  return r ? 3 : 0;
}



/* ---------------- live view: append-only 8-bit RGB rows for the GUI ----------------
   file = 16-byte header {"V3LV", u32 width, u32 expected_rows, u32 decimation} + rows.
   Readers use only complete rows (file size); flushed about every 0.3 s. */
typedef struct { FILE *f; int dec, win, wout, bits; uint8_t *row; struct timespec last; } live_t;
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static live_t *live_open(const char *dir, const char *name, int width, int h, int bits, int dec) {
  if (!dir) return NULL;
  char p[1100]; snprintf(p, sizeof p, "%s/%s", dir, name);
  live_t *lv = calloc(1, sizeof *lv); if (!lv) return NULL;
  lv->f = fopen(p, "wb"); if (!lv->f) { free(lv); return NULL; }
  lv->dec = dec < 1 ? 1 : dec; lv->win = width; lv->wout = width / lv->dec; lv->bits = bits;
  lv->row = malloc((size_t)lv->wout * 3);
  uint8_t hd[16]; memcpy(hd, "V3LV", 4); le32(hd + 4, lv->wout); le32(hd + 8, (h + lv->dec - 1) / lv->dec); le32(hd + 12, lv->dec);
  fwrite(hd, 1, 16, lv->f); fflush(lv->f);
  clock_gettime(CLOCK_MONOTONIC, &lv->last);
  return lv;
}
static void live_line(live_t *lv, const uint8_t *line, int index) {
  if (!lv || index % lv->dec) return;
  int bp = lv->bits / 8;
  for (int x = 0; x < lv->wout; x++)
    for (int c = 0; c < 3; c++) {
      unsigned sum = 0;
      for (int k = 0; k < lv->dec; k++) {
        size_t i = ((size_t)(x * lv->dec + k) * 3 + c) * bp;
        sum += bp == 1 ? line[i] : line[i + 1];          /* 16-bit LE: high byte */
      }
      lv->row[x * 3 + c] = (uint8_t)(sum / lv->dec);
    }
  fwrite(lv->row, 1, (size_t)lv->wout * 3, lv->f);
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  if ((now.tv_sec - lv->last.tv_sec) * 1000 + (now.tv_nsec - lv->last.tv_nsec) / 1000000 >= 300) { fflush(lv->f); lv->last = now; }
}
static void live_close(live_t *lv) { if (!lv) return; fclose(lv->f); free(lv->row); free(lv); }

typedef struct segw segw;
static int segw_line(segw *w, const uint8_t *line, int index);
typedef struct {
  segw *sw;                   /* split mode */
  live_t *lv;                 /* optional live view */
  v350_stitch *st;            /* NULL: write straight to `direct` */
  v350_tiff_stream *direct;
  v350_tiff_stream *keep;     /* optional raw window copy */
  uint8_t *mem; size_t mem_cap, mem_n;   /* optional in-memory copy (preview analysis) */
  int k, n, h, progress, step;
  fin_lut *dl; uint8_t *dbuf; int dw;   /* finished output on the direct (single 300 dpi window) path */
} tee_t;

static int tee_line(void *ctx, const uint8_t *line, size_t bpl, int index) {
  tee_t *t = ctx;
  if (t->keep && v350_tiff_write_rows(t->keep, line, 1)) return -1;
  if (t->mem) {
    if (t->mem_n + bpl > t->mem_cap) return -1;
    memcpy(t->mem + t->mem_n, line, bpl); t->mem_n += bpl;
  }
  if (t->progress && (index % t->step == 0 || index == t->h - 1)) {
    fprintf(stderr, "PROGRESS %d %d %d %d\n", t->k + 1, t->n, index + 1, t->h);
    fflush(stderr);
  }
  if (t->lv) live_line(t->lv, line, index);
  if (t->sw) return segw_line(t->sw, line, index);
  if (t->st) return stitch_line(t->st, line, bpl, index);
  if (t->dl) { fin_row(t->dl, line, t->dbuf, t->dw); return v350_tiff_write_rows(t->direct, t->dbuf, 1); }
  return v350_tiff_write_rows(t->direct, line, 1);
}

static int parse_list(const char *s, double *v, int max) {
  int n = 0;
  while (*s && n < max) {
    char *e; double d = strtod(s, &e);
    if (e == s) return -1;
    v[n++] = d; s = e;
    while (*s == ',' || *s == ' ') s++;
  }
  return n;
}

/* minimal reader for our own preview JSON: "cuts": [..] and "end": x */
static int read_cuts_json(const char *path, double *cuts, int max, double *end, double *start) {
  FILE *f = fopen(path, "r"); if (!f) return -1;
  static char buf[1 << 18]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
  char *p = strstr(buf, "\"cuts\""); if (!p || !(p = strchr(p, '['))) return -1;
  char *q = strchr(p, ']'); if (!q) return -1;
  *q = 0; int c = parse_list(p + 1, cuts, max); *q = ']';
  char *e = strstr(buf, "\"end\""); if (e && (e = strchr(e, ':'))) *end = strtod(e + 1, NULL);
  char *st = strstr(buf, "\"start\""); if (st && (st = strchr(st, ':'))) *start = strtod(st + 1, NULL);
  return c;
}

static void default_out(char *def, size_t n, const char *prefix) {
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
  snprintf(def, n, "%s/Pictures/V350/%s%04d%02d%02d_%02d%02d%02d.tif",
           getenv("HOME") ? getenv("HOME") : ".", prefix, tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void swap_ext(char *dst, size_t n, const char *path, const char *ext) {
  snprintf(dst, n, "%s", path);
  char *dot = strrchr(dst, '.'), *sl = strrchr(dst, '/');
  if (dot && (!sl || dot > sl)) *dot = 0;
  strncat(dst, ext, n - strlen(dst) - 1);
}


/* read back one of our own uncompressed, single-strip, little-endian TIFFs */
static uint8_t *read_own_tiff(const char *path, int *w, int *h, int *bits, int *spp) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  uint8_t hd[8]; if (fread(hd, 1, 8, f) != 8 || hd[0] != 'I' || hd[1] != 'I') { fclose(f); return NULL; }
  uint32_t ifd = hd[4] | hd[5] << 8 | hd[6] << 16 | (uint32_t)hd[7] << 24;
  fseek(f, ifd, SEEK_SET);
  uint8_t nb[2]; if (fread(nb, 1, 2, f) != 2) { fclose(f); return NULL; }
  int n = nb[0] | nb[1] << 8; uint32_t off = 0, comp = 1;
  *w = *h = 0; *bits = 8; *spp = 1;
  for (int i = 0; i < n; i++) {
    uint8_t e[12]; if (fread(e, 1, 12, f) != 12) { fclose(f); return NULL; }
    int tag = e[0] | e[1] << 8, type = e[2] | e[3] << 8;
    uint32_t cnt = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
    uint32_t v = type == 3 ? (uint32_t)(e[8] | e[9] << 8) : (e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24);
    if (tag == 256) *w = v; else if (tag == 257) *h = v; else if (tag == 259) comp = v;
    else if (tag == 277) *spp = v; else if (tag == 273) off = v;
    else if (tag == 258) {
      if (cnt == 1) *bits = v;
      else { long here = ftell(f); uint8_t b2[2]; fseek(f, (long)(e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24), SEEK_SET);
             if (fread(b2, 1, 2, f) == 2) *bits = b2[0] | b2[1] << 8;
             fseek(f, here, SEEK_SET); }
    }
  }
  if (comp != 1 || !off || *w <= 0 || *h <= 0) { fclose(f); return NULL; }
  size_t sz = (size_t)*w * *h * *spp * (*bits / 8);
  uint8_t *buf = malloc(sz);
  fseek(f, off, SEEK_SET);
  if (!buf || fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return NULL; }
  fclose(f);
  return buf;
}

/* --analyze P.tif: gap detection + PNG + JSON for an existing 300 dpi preview TIFF (no scanner) */
static int cmd_analyze(const char *path) {
  int w, h, bits, spp;
  uint8_t *img = read_own_tiff(path, &w, &h, &bits, &spp);
  if (!img || spp != 3) { fprintf(stderr, "cannot read %s (need our own RGB TIFF)\n", path); free(img); return 1; }
  pv_info pi; char png[1024], json[1024];
  pv_analyze(img, w, h, bits, 300, PREVIEW_SPLIT, g_film, &pi);
  swap_ext(png, sizeof png, path, ".png"); swap_ext(json, sizeof json, path, ".json");
  if (pv_write_png(png, img, w, h, bits, g_film, &pi) || pv_write_json(json, &pi, path, png)) { fprintf(stderr, "write failed\n"); free(img); return 5; }
  pv_print(&pi);
  printf("png: %s\njson: %s\n", png, json);
  pv_free(&pi);
  free(img);
  return 0;
}

/* --develop IN OUT: apply the finishing pipeline to an already scanned raw TIFF (offline) */
static int cmd_develop(const char *in, const char *out, const v350_finish *f, int dpi) {
  int w, h, bits, spp;
  uint8_t *img = read_own_tiff(in, &w, &h, &bits, &spp);
  if (!img || spp != 3) { fprintf(stderr, "cannot read %s (need our own raw RGB TIFF)\n", in); free(img); return 1; }
  fin_lut L; if (fin_lut_build(&L, f, bits)) { free(img); return 5; }
  size_t rb = (size_t)w * 3 * (bits / 8);
  uint8_t *o = malloc(rb);
  v350_tiff_stream *t = v350_tiff_open(out, w, bits, f->bw ? 1 : 3, dpi);
  int rc = (!o || !t) ? 5 : 0;
  for (int y = 0; y < h && !rc; y++) { fin_row(&L, img + rb * y, o, w); rc = v350_tiff_write_rows(t, o, 1) ? 5 : 0; }
  if (t) v350_tiff_close(t, NULL);
  free(o); free(img); fin_lut_free(&L);
  if (!rc) printf("%s\n", out);
  return rc;
}

/* ---------------- split mode: one trimmed TIFF per segment ---------------- */
struct segw {
  v350_tiff_stream *t;
  int w, bits, dpi, h; size_t rb;
  int trim_a, trim_b;          /* trim at start / end cut? */
  int exp_a, exp_b;            /* expected cut lines in this window */
  int snapR, pad, maxgap, edge, a_ctr, b_ctr;
  uint8_t *head; int head_n, head_need;
  uint8_t *tail; int tail_n, hold_from;
  int started, a_line, b_line, a_found, b_found;
  int a_g0, a_g1;              /* start gap rows (window lines) when found */
  const v350_finish *fin;      /* 「输出成片」: finishing parameters (NULL = raw) */
  fin_lut L; int lut_ready; uint8_t *obuf;
};
static double G_fin_carry[3];  /* film base measured in the previous window (finished output) */

static double row_level(const segw *w, const uint8_t *r) {
  int x0 = w->w / 5, x1 = w->w * 4 / 5, step = w->w / 200 > 0 ? w->w / 200 : 1; double sum = 0; int n = 0;
  for (int x = x0; x < x1; x += step) {
    size_t i = (size_t)x * 3 + 1;
    sum += w->bits == 8 ? r[i] / 255.0 : (r[2 * i] | (r[2 * i + 1] << 8)) / 65535.0; n++;
  }
  return sum / n;
}
/* negatives: row brightness if the row is uniform (p10 >= 0.85 * p90, i.e. film base), else 0 */
static double row_base(const segw *w, const uint8_t *r) {
  int x0 = w->w / 5, x1 = w->w * 4 / 5, step = w->w / 200 > 0 ? w->w / 200 : 1, n = 0;
  double v[256];
  for (int x = x0; x < x1 && n < 256; x += step) {
    size_t i = (size_t)x * 3 + 1;
    v[n++] = w->bits == 8 ? r[i] / 255.0 : (r[2 * i] | (r[2 * i + 1] << 8)) / 65535.0;
  }
  if (n < 4) return 0;
  qsort(v, n, sizeof(double), cmpdbl);
  double p10 = v[n / 10], p90 = v[n * 9 / 10], med = v[n / 2];
  if (med >= 0.9) return 0;                               /* no film (lamp) */
  return p10 >= 0.85 * p90 ? med : 0;
}
/* rows[0..n) are window lines base..base+n. Returns centre of the gap near `exp` (window line),
   or exp if none / if the gap-like run is a long leader. Positives: black gap. Negatives: a uniform
   band at film-base level (the brightest uniform rows near the expected cut). */
static int find_gap(const segw *w, const uint8_t *rows, int n, int base, int exp, int *found, int *g0, int *g1) {
  *found = 0; *g0 = *g1 = exp;
  int neg = (g_film == PV_CNEG || g_film == PV_BNEG);
  double *v = malloc(sizeof(double) * (n > 0 ? n : 1)); if (!v) return exp;
  int sm = w->dpi / 200 > 0 ? w->dpi / 200 : 1;
  for (int i = 0; i < n; i++) v[i] = neg ? row_base(w, rows + (size_t)i * w->rb) : row_level(w, rows + (size_t)i * w->rb);
  double best = neg ? -1 : 1e9; int bi = -1;
  for (int L = exp - w->snapR; L <= exp + w->snapR; L++) {
    int i = L - base; if (i - sm < 0 || i + sm >= n) continue;
    double a = 0; for (int j = i - sm; j <= i + sm; j++) a += v[j]; a /= 2 * sm + 1;
    if (neg ? a > best : a < best) { best = a; bi = i; }
  }
  int res = exp;
  int ok = bi >= 0 && (neg ? best > 0.01 : best < 0.05);
  if (ok) {
    double thr = neg ? 0.9 * best : 0.05;
    int r0 = bi, r1 = bi;
    if (neg) { while (r0 > 0 && v[r0 - 1] >= thr) r0--; while (r1 < n - 1 && v[r1 + 1] >= thr) r1++; }
    else     { while (r0 > 0 && v[r0 - 1] < thr) r0--;  while (r1 < n - 1 && v[r1 + 1] < thr) r1++; }
    int touches = (r0 == 0 || r1 == n - 1);
    if (r1 - r0 + 1 <= w->maxgap && !touches) { res = base + (r0 + r1) / 2; *found = 1; *g0 = base + r0; *g1 = base + r1; }
  }
  free(v);
  return res;
}
/* finished output: build the LUT once per window. The strip-wide parameters come from the
   preview; only the film-base level is re-measured in this window's start gap (the regular scan
   path may expose slightly differently from the 300 dpi preview path). Densities are relative to
   the base, so the black/white points stay valid. */
static int segw_lut(segw *w) {
  v350_finish f = *w->fin;
  double m[3] = {0, 0, 0}; long cnt = 0;
  if (w->a_found && w->a_g1 - w->a_g0 >= 2 * w->edge + 2) {
    int x0 = w->w / 5, x1 = w->w * 4 / 5, st = w->w / 200 > 0 ? w->w / 200 : 1;
    for (int y = w->a_g0 + w->edge; y <= w->a_g1 - w->edge; y++) {
      const uint8_t *r = w->head + (size_t)y * w->rb;
      for (int x = x0; x < x1; x += st, cnt++) for (int c = 0; c < 3; c++) {
        size_t i = (size_t)x * 3 + c;
        m[c] += w->bits == 8 ? r[i] / 255.0 : (r[2 * i] | (r[2 * i + 1] << 8)) / 65535.0;
      }
    }
  }
  const char *src = "preview";
  if (cnt > 20) {
    int ok = 1;
    for (int c = 0; c < 3; c++) { m[c] /= cnt; double q = m[c] / f.base[c]; if (q < 0.5 || q > 2) ok = 0; }
    if (f.bw) { double a = (m[0] + m[1] + m[2]) / 3; m[0] = m[1] = m[2] = a; }
    if (ok) { memcpy(G_fin_carry, m, sizeof m); src = "this window's start gap"; }
  }
  if (G_fin_carry[1] > 0) { memcpy(f.base, G_fin_carry, sizeof f.base); if (cnt <= 20) src = "previous window"; }
  fprintf(stderr, "finish: %s, base %.4f %.4f %.4f (from %s)\n", f.bw ? "B&W greyscale" : "colour",
          f.base[0], f.base[1], f.base[2], src);
  if (fin_lut_build(&w->L, &f, w->bits)) return -1;
  w->obuf = malloc(w->rb); if (!w->obuf) return -1;
  w->lut_ready = 1; return 0;
}
static int segw_put(segw *w, const uint8_t *rows, int n) {
  if (n <= 0) return 0;
  if (!w->fin) return v350_tiff_write_rows(w->t, rows, n);
  if (!w->lut_ready && segw_lut(w)) return -1;
  for (int i = 0; i < n; i++) {
    fin_row(&w->L, rows + (size_t)i * w->rb, w->obuf, w->w);
    if (v350_tiff_write_rows(w->t, w->obuf, 1)) return -1;
  }
  return 0;
}
static void segw_free(segw *w) { if (w->lut_ready) fin_lut_free(&w->L); free(w->obuf); w->obuf = NULL; w->lut_ready = 0; }
static int segw_line(segw *w, const uint8_t *line, int index) {
  if (!w->started) {                                     /* collecting head to locate the start cut */
    memcpy(w->head + (size_t)w->head_n * w->rb, line, w->rb);
    if (++w->head_n < w->head_need) return 0;
    if (w->trim_a) {
      int g0, g1, c = find_gap(w, w->head, w->head_n, 0, w->exp_a, &w->a_found, &g0, &g1);
      w->a_ctr = c; w->a_g0 = g0; w->a_g1 = g1;
      w->a_line = w->a_found ? g0 - w->edge : c - w->pad;   /* whole gap kept; model: safe pad */
    } else w->a_line = 0;
    if (w->a_line < 0) w->a_line = 0;
    w->started = 1;
    if (w->a_line < w->head_n) return segw_put(w, w->head + (size_t)w->a_line * w->rb, w->head_n - w->a_line);
    return 0;
  }
  if (index >= w->hold_from) {
    memcpy(w->tail + (size_t)(index - w->hold_from) * w->rb, line, w->rb);
    w->tail_n = index - w->hold_from + 1;
    return 0;
  }
  if (index >= w->a_line) return segw_put(w, line, 1);
  return 0;
}
/* complete: locate the end cut in the held tail and write up to it */
static int segw_finish(segw *w) {
  if (!w->started) {                                     /* window shorter than head (should not happen) */
    w->started = 1; w->a_line = 0;
    if (segw_put(w, w->head, w->head_n)) return -1;
  }
  int stop = w->hold_from + w->tail_n;
  if (w->trim_b && w->tail_n > 0) {
    int g0, g1, g = find_gap(w, w->tail, w->tail_n, w->hold_from, w->exp_b, &w->b_found, &g0, &g1);
    w->b_ctr = g;
    stop = w->b_found ? g1 + w->edge + 1 : g + w->pad + 1;
    if (stop > w->hold_from + w->tail_n) stop = w->hold_from + w->tail_n;
  }
  w->b_line = stop;
  int from = w->a_line > w->hold_from ? w->a_line - w->hold_from : 0;
  return segw_put(w, w->tail + (size_t)from * w->rb, stop - w->hold_from - from);
}


static const char *G_OUTDIR = NULL;   /* --outdir */
static v350_finish G_FIN;             /* --finish: parameters from the preview JSON */
static int G_FINISH = 0;
static int G_ESCN1 = 0;               /* --esc-n1: scan negatives with the scanner's negative mode */
static int scan_film_mode(void) { return G_ESCN1 && (g_film == PV_CNEG || g_film == PV_BNEG) ? V350_FILM_NEG : V350_FILM_POS; }
static int G_CONT = 0;                /* --continue-numbering (implied by --outdir; kept for compatibility) */
/* 1 + highest NN of files named frame_NN.<ext> (any ext, case-insensitive), 1 if none */
static int next_frame_number(const char *dir) {
  int mx = 0; DIR *d = opendir(dir); if (!d) return 1;
  struct dirent *e;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    if (strncasecmp(n, "frame_", 6)) continue;
    const char *p = n + 6; if (!isdigit((unsigned char)*p)) continue;
    int v = 0; while (isdigit((unsigned char)*p) && v < 1000000) v = v * 10 + (*p++ - '0');
    if (*p == '.' || *p == '_') { if (v > mx) mx = v; }
  }
  closedir(d);
  return mx + 1;
}

static int run_split(int dpi, int bits, int width, int nwin, const int *Tl, const int *El, const int *Xl,
                     const double *b, int ml, int Kl, const int *sel, double pad_in, const char *out,
                     const char *keepdir, const char *livedir, int progress, int verbose) {
  char dir[900];
  int cont = G_CONT;
  if (out) snprintf(dir, sizeof dir, "%s", out);                 /* --out: exact folder, frame_<segment> */
  else {                                                         /* --outdir / default: straight into the */
    cont = 1;                                                    /* folder, numbering continues            */
    if (G_OUTDIR) snprintf(dir, sizeof dir, "%s", G_OUTDIR);
    else snprintf(dir, sizeof dir, "%s/Pictures/V350", getenv("HOME") ? getenv("HOME") : ".");
  }
  char probe[1000]; snprintf(probe, sizeof probe, "%s/x", dir);
  if (ensure_parent(probe) != 0) { fprintf(stderr, "cannot create %s\n", dir); return 1; }
  if (keepdir && mkdir(keepdir, 0755) && errno != EEXIST) { fprintf(stderr, "cannot create %s\n", keepdir); return 1; }
  int nsel = 0; for (int k = 0; k < nwin; k++) nsel += sel[k];
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sigint;
  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

  v350_t s; memset(&s, 0, sizeof s);
  s.verbose = verbose; s.require_high_speed = (dpi >= 1200);
  if (v350_open(&s) != 0) { fprintf(stderr, "open: %s\n", s.err); return 1; }
  size_t rb = (size_t)width * 3 * (bits / 8);
  int snapR = (int)lround(SNAP_IN * dpi), pad = (int)lround(pad_in * dpi), maxgap = (int)lround(0.25 * dpi);
  int span = ml + snapR + maxgap;                          /* rows needed around each cut */
  uint8_t *head = malloc(rb * span), *tail = malloc(rb * (2 * span + 1));
  if (!head || !tail) { fprintf(stderr, "oom\n"); v350_close(&s); return 5; }
  time_t t0 = time(NULL);
  int rc = 0, ord = 0, saved = 0;
  double bias = 0;                                         /* lines: raw position error predicted for next window */
  for (int k = 0; k < nwin && rc == 0; k++) {
    if (!sel[k]) { fprintf(stderr, "SEGMENT %d skipped\n", k + 1); continue; }
    int h = El[k] - Tl[k];
    int y = (int)lround((Tl[k] - Kl - bias) / F_WIN); if (y < 0) y = 0;
    char path[1100];
    if (cont) {                                      /* next free number after the highest frame_NN.* */
      int n = next_frame_number(dir);
      for (;; n++) { snprintf(path, sizeof path, "%s/frame_%02d.tif", dir, n); if (access(path, F_OK) != 0) break; }
    } else {
      snprintf(path, sizeof path, "%s/frame_%02d.tif", dir, k + 1);
      for (int v = 2; access(path, F_OK) == 0; v++)  /* never overwrite */
        snprintf(path, sizeof path, "%s/frame_%02d_%d.tif", dir, k + 1, v);
    }
    fprintf(stderr, "== segment %d (%.3f\"..%.3f\" preview): window y=%d h=%d (%.3f\") bias %+.4f\" ==\n",
            k + 1, b[k], b[k + 1], y, h, h / (double)dpi, bias / dpi);
    fprintf(stderr, "SEGMENT %d scanning\n", k + 1); fflush(stderr);
    if (v350_prepare_afl(&s) != 0) { fprintf(stderr, "prepare: %s\n", s.err); rc = v350_is_wedged() ? 4 : 2; fprintf(stderr, "SEGMENT %d failed\n", k + 1); break; }
    segw w; memset(&w, 0, sizeof w);
    w.fin = G_FINISH ? &G_FIN : NULL;
    w.t = v350_tiff_open(path, width, bits, w.fin && w.fin->bw ? 1 : 3, dpi);
    if (!w.t) { fprintf(stderr, "cannot create %s\n", path); rc = 5; break; }
    w.w = width; w.bits = bits; w.dpi = dpi; w.h = h; w.rb = rb;
    w.trim_a = b[k] > 0.01; w.trim_b = 1;
    w.exp_a = Xl[k] - Tl[k]; w.exp_b = Xl[k + 1] - Tl[k];
    w.snapR = snapR; w.pad = pad; w.maxgap = maxgap; w.edge = dpi / 200 > 1 ? dpi / 200 : 1;
    w.head = head; w.head_need = w.trim_a ? (w.exp_a + snapR + maxgap < h ? w.exp_a + snapR + maxgap : h) : 1;
    w.tail = tail; w.hold_from = w.exp_b - snapR - maxgap;
    if (w.hold_from < w.head_need) w.hold_from = w.head_need;
    if (h - w.hold_from > 2 * span + 1) w.hold_from = h - (2 * span + 1);
    tee_t tee; memset(&tee, 0, sizeof tee);
    char lname[32]; snprintf(lname, sizeof lname, "live_%02d.raw", k + 1);
    tee.lv = live_open(livedir, lname, width, h, bits, dpi / 75);
    tee.sw = &w; tee.progress = progress; tee.k = ord; tee.n = nsel; tee.h = h; tee.step = h / 100 > 0 ? h / 100 : 1;
    if (keepdir) {
      char kp[1100]; snprintf(kp, sizeof kp, "%s/win%02d.tif", keepdir, k + 1);
      tee.keep = v350_tiff_open(kp, width, bits, 3, dpi);
    }
    int gw = 0, gh = 0; time_t ts0 = time(NULL);
    v350_rc_t r = v350_scan_window(&s, dpi, bits, V350_COLOR_RGB, scan_film_mode(), y, h, tee_line, &tee, &gw, &gh);
    if (tee.keep) v350_tiff_close(tee.keep, NULL);
    live_close(tee.lv); tee.lv = NULL;
    int fin = segw_finish(&w);
    segw_free(&w);
    int H = 0; int cl = v350_tiff_close(w.t, &H);
    ord++;
    if (r != V350_OK || fin || cl || gw != width || gh != h) {
      fprintf(stderr, "SCAN FAILED in segment %d: %s (partial file kept: %s, %d rows)\n", k + 1,
              r != V350_OK ? s.err : "geometry/file error", path, H);
      fprintf(stderr, "SEGMENT %d failed\n", k + 1);
      rc = (r == V350_ERR_WEDGED) ? 4 : (r != V350_OK ? 3 : 5); break;
    }
    /* measured window position error from the gaps that were found */
    double e = 0; int ne = 0;
    if (w.a_found) { e += w.exp_a - w.a_ctr; ne++; }
    if (w.b_found) { e += w.exp_b - w.b_ctr; ne++; }
    if (ne) bias += e / ne;
    fprintf(stderr, "segment %d: kept window lines %d..%d of %d (start %s, end %s), position error %s%+.4f\", %ld s\n",
            k + 1, w.a_line, w.b_line, h,
            !w.trim_a ? "strip start" : w.a_found ? "gap found" : "model (no gap found)",
            w.b_found ? "gap found" : "model (no gap found)",
            ne ? "" : "n/a ", ne ? e / ne / dpi : 0.0, (long)(time(NULL) - ts0));
    fprintf(stderr, "SEGMENT %d saved %s\n", k + 1, path); fflush(stderr);
    saved++;
  }
  free(head); free(tail);
  if (!v350_is_wedged()) { if (v350_wait_ready(&s, 120) == 0) v350_release(&s); }
  else wedged_msg();
  v350_close(&s);
  fprintf(stderr, "%s: %d of %d selected segment(s) saved in %ld s -> %s\n", rc ? "STOPPED" : "OK", saved, nsel,
          (long)(time(NULL) - t0), dir);
  printf("%s\n", dir);
  return rc;
}

int main(int argc, char **argv) {
  int dpi = 0, bits = 16, verbose = 1, preview = 0, progress = 0, dry = 0, bits_set = 0, stitch = 0;
  int only[MAXW]; int nonly = 0; double pad_in = PAD_IN;
  const char *out = NULL, *keepdir = NULL, *cutsfrom = NULL, *livedir = NULL, *analyze = NULL;
  int check_driver = 0; const char *finfrom = NULL, *dev_in = NULL, *dev_out = NULL;
  double cuts[MAXW]; int ncuts = 0;
  double start = -1, end = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dpi") && i + 1 < argc) dpi = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--bits") && i + 1 < argc) { bits = atoi(argv[++i]); bits_set = 1; }
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--preview")) preview = 1;
    else if (!strcmp(argv[i], "--cuts") && i + 1 < argc) {
      ncuts = parse_list(argv[++i], cuts, MAXW - 2);
      if (ncuts < 0) { fprintf(stderr, "bad --cuts list\n"); return 1; }
    }
    else if (!strcmp(argv[i], "--cuts-from") && i + 1 < argc) cutsfrom = argv[++i];
    else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atof(argv[++i]);
    else if (!strcmp(argv[i], "--end") && i + 1 < argc) end = atof(argv[++i]);
    else if (!strcmp(argv[i], "--keep-segments") && i + 1 < argc) keepdir = argv[++i];
    else if (!strcmp(argv[i], "--progress")) progress = 1;
    else if (!strcmp(argv[i], "--stitch")) stitch = 1;
    else if (!strcmp(argv[i], "--live") && i + 1 < argc) livedir = argv[++i];
    else if (!strcmp(argv[i], "--outdir") && i + 1 < argc) G_OUTDIR = argv[++i];
    else if (!strcmp(argv[i], "--continue-numbering")) G_CONT = 1;
    else if (!strcmp(argv[i], "--split")) stitch = 0;
    else if (!strcmp(argv[i], "--pad") && i + 1 < argc) pad_in = atof(argv[++i]);
    else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
      double v[MAXW]; int n = parse_list(argv[++i], v, MAXW);
      if (n <= 0) { fprintf(stderr, "bad --only list\n"); return 1; }
      for (int j = 0; j < n; j++) only[nonly++] = (int)v[j];
    }
    else if (!strcmp(argv[i], "--dry-run")) dry = 1;
    else if (!strcmp(argv[i], "--cal") && i + 1 < argc) {
      double v[3]; if (parse_list(argv[++i], v, 3) != 3) { fprintf(stderr, "bad --cal\n"); return 1; }
      F_PREV = v[0]; F_WIN = v[1]; K_IN = v[2];
    }
    else if (!strcmp(argv[i], "--status")) return cmd_status(verbose);
    else if (!strcmp(argv[i], "--analyze") && i + 1 < argc) analyze = argv[++i];
    else if (!strcmp(argv[i], "--film") && i + 1 < argc) {
      g_film = pv_film_from_name(argv[++i]);
      if (g_film < 0) { fprintf(stderr, "bad --film (cpos|bpos|cneg|bneg)\n"); return 1; }
    }
    else if (!strcmp(argv[i], "--interpreter") && i + 1 < argc) v350_set_interpreter(argv[++i]);
    else if (!strcmp(argv[i], "--check-driver")) check_driver = 1;
    else if (!strcmp(argv[i], "--finish")) G_FINISH = 1;
    else if (!strcmp(argv[i], "--finish-from") && i + 1 < argc) { G_FINISH = 1; finfrom = argv[++i]; }
    else if (!strcmp(argv[i], "--esc-n1")) G_ESCN1 = 1;
    else if (!strcmp(argv[i], "--develop") && i + 2 < argc) { dev_in = argv[i + 1]; dev_out = argv[i + 2]; i += 2; }
    else if (!strcmp(argv[i], "--experimental-eject")) return cmd_eject_experimental(verbose);
    else if (!strcmp(argv[i], "-v")) verbose++;
    else if (!strcmp(argv[i], "-q")) verbose = 0;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
  }
  if (check_driver) {                 /* no USB access */
    const char *b = v350_find_interpreter();
    if (!b) { printf("DRIVER_MISSING\n"); fprintf(stderr, "Interpreter 68.bundle not found. Install Epson ICA Scanner Driver 5.8.23:\n"
                     "  https://epson.com/Support/Scanners/Perfection-Series/Epson-Perfection-V350-Photo/s/SPT_B11B185011\n"); return 2; }
    printf("DRIVER_OK %s\n", b);
    printf("FIRMWARE %s\n", v350_firmware_present(b) ? "OK" : "MISSING");
    return v350_firmware_present(b) ? 0 : 3;
  }
  if (analyze) return cmd_analyze(analyze);
  if (dev_in) {
    static char jb[1 << 18]; size_t jn = 0; v350_finish f;
    FILE *jf = finfrom ? fopen(finfrom, "r") : NULL;
    if (jf) { jn = fread(jb, 1, sizeof jb - 1, jf); fclose(jf); }
    jb[jn] = 0;
    if (!finfrom || fin_from_json(&f, jb)) { fprintf(stderr, "--develop needs --finish-from PREVIEW.json (negative preview)\n"); return 1; }
    return cmd_develop(dev_in, dev_out, &f, dpi > 0 ? dpi : 300);
  }
  if (preview) { dpi = 300; if (!bits_set) bits = 8; }
  if (!(dpi == 300 || dpi == 600 || dpi == 1200 || dpi == 2400 || dpi == 4800)) {
    fprintf(stderr, "need --preview or --dpi 300|600|1200|2400|4800 (AFL has no 3200)\n"); return 1;
  }
  if (bits != 8 && bits != 16) { fprintf(stderr, "--bits must be 8 or 16\n"); return 1; }
  if (cutsfrom) {
    double e2 = -1, s2 = -1;
    ncuts = read_cuts_json(cutsfrom, cuts, MAXW - 2, &e2, &s2);
    if (start < 0 && s2 >= 0) start = s2;
    if (ncuts < 0) { fprintf(stderr, "cannot read cuts from %s\n", cutsfrom); return 1; }
    if (end < 0) end = e2;
  }
  if (G_FINISH && preview) { fprintf(stderr, "note: --finish is ignored for --preview (preview is always raw; its PNG is finished)\n"); G_FINISH = 0; }
  if (G_FINISH && !(g_film == PV_CNEG || g_film == PV_BNEG)) {
    fprintf(stderr, "note: --finish only applies to negatives (--film cneg|bneg); saving raw\n"); G_FINISH = 0;
  }
  if (G_FINISH) {
    const char *src = finfrom ? finfrom : cutsfrom;
    static char jb[1 << 18]; size_t jn = 0;
    FILE *jf = src ? fopen(src, "r") : NULL;
    if (jf) { jn = fread(jb, 1, sizeof jb - 1, jf); fclose(jf); }
    jb[jn] = 0;
    if (!src || fin_from_json(&G_FIN, jb)) {
      fprintf(stderr, "--finish needs the strip's preview JSON (--finish-from P.json or --cuts-from P.json) "
                      "with a \"finish\" block (take the preview with --film cneg|bneg)\n"); return 1;
    }
    if ((g_film == PV_BNEG) != (G_FIN.bw != 0)) {
      fprintf(stderr, "--finish: preview was taken with a different film type; take a new preview\n"); return 1;
    }
    fprintf(stderr, "finish: %s, strip parameters base %.4f %.4f %.4f  black %.3f %.3f %.3f  white %.3f %.3f %.3f D  S %.2f gamma %.2f\n",
            G_FIN.bw ? "B&W -> greyscale" : "colour", G_FIN.base[0], G_FIN.base[1], G_FIN.base[2],
            G_FIN.dlo[0], G_FIN.dlo[1], G_FIN.dlo[2], G_FIN.dhi[0], G_FIN.dhi[1], G_FIN.dhi[2], G_FIN.S, G_FIN.gamma);
  }
  if (G_ESCN1) fprintf(stderr, "note: --esc-n1 (EXPERIMENTAL, not hardware-tested): %s\n",
                       (g_film == PV_CNEG || g_film == PV_BNEG) ? "scanning with ESC N=1 (scanner negative mode)" : "ignored for positives");
  if (start < 0) start = 0;
  double maxwin = V350_AFL_H100 / 100.0;                 /* 9.30" window-coordinate limit */
  if (end < 0) end = maxwin;
  int width = (V350_AFL_W100 * dpi / 100) / 8 * 8;

  /* ---------------- window plan (lines at dpi, true scale) ---------------- */
  int nwin = 1, Tl[MAXW], El[MAXW], Xl[MAXW + 1];
  double b[MAXW + 1]; int nb = 0; int psel[MAXW];
  for (int k = 0; k < MAXW; k++) psel[k] = 1;
  int ml = (int)lround(MARGIN_IN * dpi), Kl = (int)lround(K_IN * dpi);
  if (dpi == 300) {
    if (ncuts) fprintf(stderr, "note: 300 dpi is always one whole-strip window; cuts ignored\n");
    ncuts = 0; nwin = 1; Tl[0] = 0; El[0] = V350_AFL_H100 * dpi / 100;
  } else {
    b[nb++] = start;
    for (int i = 0; i < ncuts; i++) if (cuts[i] > start + 0.05 && cuts[i] < end - 0.05) b[nb++] = cuts[i];
    b[nb++] = end;
    /* which segments are scanned (--only); only those are validated */
    for (int k = 0; k < nb - 1; k++) psel[k] = nonly ? 0 : 1;
    for (int j = 0; j < nonly; j++) {
      if (only[j] < 1 || only[j] > nb - 1) { fprintf(stderr, "--only %d: there are %d segments\n", only[j], nb - 1); return 1; }
      psel[only[j] - 1] = 1;
    }
    for (int i = 1; i < nb; i++)
      if (psel[i - 1] && b[i] - b[i - 1] < 0.35) {
        fprintf(stderr, "segment %d (%.3f..%.3f) is shorter than 0.35\"\n", i, b[i - 1], b[i]); return 1;
      }
    if (nb == 2 && end - start > 1.9 && !ncuts) {
      fprintf(stderr, "above 300 dpi give --cuts (inches from the preview top, in the gaps between frames)\n"
                      "or --cuts-from preview.json; run  %s --preview  first.\n", argv[0]);
      return 1;
    }
    nwin = nb - 1;
    for (int i = 0; i < nb; i++) Xl[i] = (int)lround(b[i] * F_PREV * dpi);
    int limit = V350_AFL_H100 * dpi / 100;
    for (int k = 0; k < nwin; k++) {
      Tl[k] = Xl[k] - ml; El[k] = Xl[k + 1] + ml;
      if (Tl[k] < Kl) Tl[k] = Kl;
      if (psel[k] && k > 0 && Tl[k] != Xl[k] - ml) { fprintf(stderr, "segment %d: cut %.3f too close to the strip start\n", k + 1, b[k]); return 1; }
      /* window y units: (T - K)/F_WIN; y + h must stay within the 9.30" area */
      int y0 = (int)lround((Tl[k] - Kl) / F_WIN);
      if (y0 + (El[k] - Tl[k]) > limit) {
        if (k == nwin - 1 || !psel[k]) El[k] = Tl[k] + (limit - y0);
        else { fprintf(stderr, "segment %d exceeds the 9.30\" AFL area\n", k + 1); return 1; }
      }
      double len = (El[k] - Tl[k]) / (double)dpi;
      if (psel[k] && len > REFUSE_LEN) {
        fprintf(stderr, "segment %d would be %.2f\" long (> %.1f\": the regular AFL path fails on long windows).\n"
                        "Add cuts so that every segment is about one frame.\n", k + 1, len, REFUSE_LEN);
        return 1;
      }
    }
  }
  long est_h = El[nwin - 1] - Tl[0];
  fprintf(stderr, "plan: %d dpi %d-bit, %d window(s), width %d px, output ~%ld lines (%.0f MB)\n",
          dpi, bits, nwin, width, est_h, (double)width * est_h * 3 * (bits / 8) / 1e6);
  for (int k = 0; k < nwin; k++) {
    double len = (El[k] - Tl[k]) / (double)dpi;
    fprintf(stderr, "  window %d: true %.3f\"..%.3f\" (%.3f\"%s)  ESC A y~%d h=%d\n", k + 1,
            Tl[k] / (double)dpi, El[k] / (double)dpi, len, len > WARN_LEN ? ", LONG" : "",
            dpi == 300 ? 0 : (int)lround((Tl[k] - Kl) / F_WIN), El[k] - Tl[k]);
  }
  int split = (dpi > 300 && !stitch);
  int sel[MAXW];
  for (int k = 0; k < nwin; k++) sel[k] = nonly ? 0 : 1;
  for (int j = 0; j < nonly; j++) if (only[j] >= 1 && only[j] <= nwin) sel[only[j] - 1] = 1;
  if (nonly && !split) { fprintf(stderr, "--only needs split mode (not --stitch, not 300 dpi)\n"); return 1; }
  if (split) {
    fprintf(stderr, "split mode: one TIFF per segment; each keeps the whole black gap at its cuts (+%.3f\" pad if no gap is found); segments:", pad_in);
    for (int k = 0; k < nwin; k++) fprintf(stderr, " %d%s", k + 1, sel[k] ? "" : "(skip)");
    fprintf(stderr, "\n");
  }
  if (dry) return 0;
  if (split) return run_split(dpi, bits, width, nwin, Tl, El, Xl, b, ml, Kl, sel, pad_in, out, keepdir, livedir, progress, verbose);

  char def[512], png[600], json[600];
  if (!out) { default_out(def, sizeof def, preview ? "preview_" : ""); out = def; }
  if (ensure_parent(out) != 0) { fprintf(stderr, "cannot create directory for %s\n", out); return 1; }
  if (keepdir) { if (mkdir(keepdir, 0755) && errno != EEXIST) { fprintf(stderr, "cannot create %s\n", keepdir); return 1; } }

  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sigint;
  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

  v350_stitch st; int use_st = nwin > 1;
  if (G_FINISH && use_st) { fprintf(stderr, "note: --finish is not supported with --stitch; saving raw\n"); G_FINISH = 0; }
  fin_lut dl; uint8_t *dbuf = NULL;
  if (G_FINISH) {
    if (fin_lut_build(&dl, &G_FIN, bits) || !(dbuf = malloc((size_t)width * 3 * (bits / 8)))) { fprintf(stderr, "oom\n"); return 5; }
  }
  v350_tiff_stream *ts = v350_tiff_open(out, width, bits, G_FINISH && G_FIN.bw ? 1 : 3, dpi);
  if (!ts) { fprintf(stderr, "cannot create %s\n", out); return 5; }
  if (use_st && stitch_init(&st, ts, width, 3, bits, dpi, nwin, ml, verbose)) { fprintf(stderr, "oom\n"); return 5; }
  tee_t tee; memset(&tee, 0, sizeof tee);
  tee.st = use_st ? &st : NULL; tee.direct = ts; tee.progress = progress; tee.n = nwin;
  if (G_FINISH) { tee.dl = &dl; tee.dbuf = dbuf; tee.dw = width; }
  if (preview) {
    tee.mem_cap = (size_t)width * 3 * (bits / 8) * (El[0] - Tl[0]);
    tee.mem = malloc(tee.mem_cap);
    if (!tee.mem) { fprintf(stderr, "oom\n"); return 5; }
  }

  v350_t s; memset(&s, 0, sizeof s);
  s.verbose = verbose;
  s.require_high_speed = (dpi >= 1200);
  if (v350_open(&s) != 0) {
    fprintf(stderr, "open: %s\n", s.err);
    if (use_st) stitch_free(&st);
    v350_tiff_close(ts, NULL); unlink(out); return 1;
  }

  FILE *plan = NULL;
  if (keepdir) {
    char pp[1024]; snprintf(pp, sizeof pp, "%s/plan.txt", keepdir);
    plan = fopen(pp, "w");
    if (plan) fprintf(plan, "dpi %d bits %d width %d margin_lines %d cal %.4f %.4f %.4f\n# k y_req h true_start_line\n",
                      dpi, bits, width, ml, F_PREV, F_WIN, K_IN);
  }
  time_t t0 = time(NULL);
  int rc = 0;
  double bias = 0;                                  /* lines, re-estimated after every seam */
  for (int k = 0; k < nwin && rc == 0; k++) {
    int h = El[k] - Tl[k];
    int y = dpi == 300 ? 0 : (int)lround((Tl[k] - Kl - bias) / F_WIN);
    if (y < 0) y = 0;
    fprintf(stderr, "== window %d/%d: y=%d h=%d (%.3f\") bias %+.4f\" ==\n", k + 1, nwin, y, h, h / (double)dpi, bias / dpi);
    if (plan) { fprintf(plan, "%d %d %d %d\n", k + 1, y, h, Tl[k]); fflush(plan); }
    if (v350_prepare_afl(&s) != 0) {
      fprintf(stderr, "prepare: %s\n", s.err);
      rc = v350_is_wedged() ? 4 : 2; break;
    }
    if (use_st && stitch_begin_segment(&st, k, h) != 0) { fprintf(stderr, "window %d too short\n", k + 1); rc = 3; break; }
    if (keepdir) {
      char kp[1024]; snprintf(kp, sizeof kp, "%s/win%02d.tif", keepdir, k + 1);
      tee.keep = v350_tiff_open(kp, width, bits, 3, dpi);
      if (!tee.keep) { fprintf(stderr, "cannot create %s\n", kp); rc = 5; break; }
    }
    tee.k = k; tee.h = h; tee.step = h / 100 > 0 ? h / 100 : 1;
    if (livedir) { char ln[32]; snprintf(ln, sizeof ln, preview || dpi == 300 ? "live_preview.raw" : "live_w%02d.raw", k + 1);
                   tee.lv = live_open(livedir, ln, width, h, bits, dpi / 300); }
    int gw = 0, gh = 0;
    time_t ts0 = time(NULL);
    v350_rc_t r = v350_scan_window(&s, dpi, bits, V350_COLOR_RGB, scan_film_mode(), y, h, tee_line, &tee, &gw, &gh);
    if (tee.keep) { v350_tiff_close(tee.keep, NULL); tee.keep = NULL; }
    live_close(tee.lv); tee.lv = NULL;
    if (r != V350_OK) {
      fprintf(stderr, "SCAN FAILED in window %d: %s\n", k + 1, s.err);
      rc = (r == V350_ERR_WEDGED) ? 4 : 3; break;
    }
    if (gw != width || gh != h || (use_st && stitch_end_segment(&st, gh) != 0)) {
      fprintf(stderr, "window %d geometry mismatch (%dx%d)\n", k + 1, gw, gh);
      rc = 3; break;
    }
    if (use_st && k > 0) bias += st.e_cur;   /* raw error of this window */
    fprintf(stderr, "window %d done in %ld s\n", k + 1, (long)(time(NULL) - ts0));
  }
  if (plan) fclose(plan);

  if (!v350_is_wedged()) {
    if (v350_wait_ready(&s, 120) == 0) v350_release(&s);   /* ESC e 0: interpreter-local */
  } else wedged_msg();
  v350_close(&s);

  if (use_st) stitch_free(&st);
  int H = 0;
  if (v350_tiff_close(ts, &H) != 0) { fprintf(stderr, "TIFF finalise failed\n"); return 5; }
  if (use_st && rc == 0) {
    fprintf(stderr, "seams (dy lines, dx px, ncc):");
    for (int k = 1; k < nwin; k++) fprintf(stderr, " [%d] %+d %+d %.2f%s", k, st.shift[k], st.dx[k], st.ncc[k], st.flat[k] ? "(flat)" : "");
    fprintf(stderr, "\n");
    for (int k = 1; k < nwin; k++)
      if (!st.flat[k] && st.ncc[k] < 0.6)
        fprintf(stderr, "WARNING: seam %d matched poorly (ncc %.2f)\n", k, st.ncc[k]);
  }
  if (rc) { fprintf(stderr, "partial TIFF kept (%d rows): %s\n", H, out); return rc; }
  if (preview) {
    pv_info pi;
    pv_analyze(tee.mem, width, H, bits, dpi, PREVIEW_SPLIT, g_film, &pi);
    swap_ext(png, sizeof png, out, ".png"); swap_ext(json, sizeof json, out, ".json");
    if (pv_write_png(png, tee.mem, width, H, bits, g_film, &pi)) fprintf(stderr, "PNG write failed\n");
    if (pv_write_json(json, &pi, out, png)) fprintf(stderr, "JSON write failed\n");
    pv_print(&pi);
    printf("png: %s\njson: %s\n", png, json);
    pv_free(&pi);
    free(tee.mem);
  }
  fprintf(stderr, "OK in %ld s: %dx%d -> %s\n", (long)(time(NULL) - t0), width, H, out);
  printf("%s\n", out);
  return 0;
}
