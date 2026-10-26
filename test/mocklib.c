/* mock of v350lib for offline end-to-end tests of v350scan.c (no USB) */
#include "v350lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
static int wedged = 0, win = 0;
static double F_TRUE = 1.033, K_TRUE = 0.035, NOISE = 0.004, SKEW = -0.0054; /* px per line at 600 */
static double env(const char *n, double d) { const char *v = getenv(n); return v ? atof(v) : d; }
int v350_open(v350_t *s) { (void)s; F_TRUE = env("M_F", F_TRUE); K_TRUE = env("M_K", K_TRUE); NOISE = env("M_NOISE", NOISE); return 0; }
void v350_close(v350_t *s) { (void)s; }
void v350_set_interpreter(const char *p) { (void)p; }
const char *v350_find_interpreter(void) { return getenv("M_NO_DRIVER") ? NULL : "(simulator)"; }
int v350_firmware_present(const char *b) { (void)b; return 1; }
/* M_FILM=cpos|bpos|cneg|bneg: what kind of film the simulated strip is */
static int film_kind(void) {
  const char *f = getenv("M_FILM");
  if (!f) return 0;
  return !strcmp(f, "bpos") ? 1 : !strcmp(f, "cneg") ? 2 : !strcmp(f, "bneg") ? 3 : 0;
}
/* scene value v (0 = black / unexposed .. 1 = bright), lamp = no film -> raw RGB for the film kind */
static void film_rgb(double v, int lamp, int kind, double out[3]) {
  if (lamp) { out[0] = out[1] = out[2] = 1.0; return; }
  static const double cbase[3] = {0.78, 0.46, 0.21}, bbase = 0.82;
  switch (kind) {
  case 1: out[0] = out[1] = out[2] = v * 0.85; break;
  case 2: for (int c = 0; c < 3; c++) out[c] = cbase[c] * pow(10, -1.6 * v * (c == 2 ? 0.9 : 1.0)); break;
  case 3: out[0] = out[1] = out[2] = bbase * pow(10, -1.9 * v); break;
  default: out[0] = v; out[1] = v * 0.8; out[2] = v * 0.6;
  }
}
int v350_usb_speed(void) { return 3; }
int v350_is_wedged(void) { return wedged; }
int v350_status(v350_t *s, v350_status_t *st) { (void)s; memset(st, 0, sizeof *st); return 0; }
int v350_wait_ready(v350_t *s, int t) { (void)s; (void)t; return 0; }
int v350_release(v350_t *s) { (void)s; return 0; }
int v350_eject(v350_t *s) { (void)s; return 0; }
void v350_request_abort(void) {}
int v350_prepare_afl(v350_t *s) { (void)s; return 0; }
/* truth: frame pitch 1.496", gap 0.06" black; leader 0..1.6" dark; film ends 8.9" */
static double tex(double xi, double yi) {
  return 0.45 + 0.2 * sin(xi * 0.9 + yi * 0.37) * cos(yi * 1.3 - xi * 0.21) + 0.15 * sin(xi * 3.1 + 2 * sin(yi * 2.3))
         + 0.1 * cos(7.7 * xi - 5.3 * yi);
}
static double truth(double yin, double xin) {
  if (yin < 0.1 || yin > 8.9) return 1.0;               /* no film: lamp */
  if (yin < 1.6) return 0.0;                            /* leader */
  double ph = fmod(yin - 1.6, 1.496);
  if (ph < 0.06) return 0.0;                            /* inter-frame gap */
  if (xin < 0.05 || xin > 1.40) return 0.02;
  double v = tex(xin * 20, yin * 20) * (0.6 + 0.4 * sin(yin * 2));
  return v < 0 ? 0 : v > 1 ? 1 : v;
}
v350_rc_t v350_scan_window(v350_t *s, int dpi, int bits, v350_color_t color, v350_film_t film,
                           int y_px, int h_px, v350_line_cb cb, void *ctx, int *ow, int *oh) {
  (void)s; (void)color; (void)film;
  int w = (V350_AFL_W100 * dpi / 100) / 8 * 8;
  size_t bpl = (size_t)w * 3 * (bits / 8);
  uint8_t *line = calloc(1, bpl);
  /* actual start: true lines.  300 dpi = preview path, film moved by feeder (compressed). */
  double noise = (dpi == 300) ? 0 : NOISE * sin(win * 2.7 + 1) * dpi;     /* deterministic jitter */
  double start = (dpi == 300) ? 0 : K_TRUE * dpi + F_TRUE * y_px + noise;
  double yscale = (dpi == 300) ? F_TRUE : 1.0;
  int line_us = (int)(env("M_LINE_MS", 0) * 1000);          /* simulated line time */
  if (env("M_WARMUP_S", 0) > 0) usleep((int)(env("M_WARMUP_S", 0) * 1e6));
  fprintf(stderr, "[mock] window %d: y_px=%d -> true start %.1f lines (%.4f\")\n", win + 1, y_px, start, start / dpi);
  for (int j = 0; j < h_px; j++) {
    double tl = start + j * yscale;
    double yin = tl / dpi;
    double sh = SKEW * j;
    for (int x = 0; x < w; x++) {
      double xin = (x + (x >= 8 ? sh : 0)) / (double)dpi;
      double v = truth(yin, xin), rgb[3];
      film_rgb(v >= 1.0 ? 0 : v, v >= 1.0, film_kind(), rgb);
      for (int c = 0; c < 3; c++) {
        double vc = rgb[c];
        if (bits == 8) line[(size_t)x * 3 + c] = (uint8_t)(vc * 255);
        else { int q = (int)(vc * 65535); line[((size_t)x * 3 + c) * 2] = q & 255; line[((size_t)x * 3 + c) * 2 + 1] = q >> 8; }
      }
    }
    long ti = lround(tl);                                 /* encode true line in cols 0..1 */
    if (bits == 8) { line[0] = ti >> 16; line[1] = ti >> 8; line[2] = ti; line[3] = 0; line[4] = 0; line[5] = 0; }
    else { line[0] = ti >> 16; line[1] = 0; line[2] = ti >> 8; line[3] = 0; line[4] = ti & 255; line[5] = 0; }
    if (cb(ctx, line, bpl, j)) { free(line); return V350_ERR_CLEAN; }
    if (line_us) usleep(line_us);
  }
  free(line); win++;
  *ow = w; *oh = h_px;
  return V350_OK;
}
