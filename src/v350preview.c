/* v350preview.c — inter-frame gap detection, PNG and JSON for the 300 dpi preview */
#include "v350preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <math.h>

static inline double cv(const uint8_t *img, int w, int bits, int x, int y, int c) {
  size_t i = ((size_t)y * w + x) * 3 + c;
  if (bits == 8) return img[i] / 255.0;
  return (img[2 * i] | (img[2 * i + 1] << 8)) / 65535.0;
}
static inline double gv(const uint8_t *img, int w, int bits, int x, int y) { return cv(img, w, bits, x, y, 1); }
static int cmpd(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int n, double q) {      /* sorts v */
  if (n <= 0) return 0;
  qsort(v, n, sizeof(double), cmpd);
  int i = (int)(q * (n - 1) + 0.5); return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

#define DARK 0.01     /* positives: row 90th percentile below this = black gap (fraction of full scale) */
#define NOFILM 0.9    /* row median above this = no film in the light path (strip ends) */
#define NEG_LO 0.85   /* negatives: row 10th percentile >= NEG_LO * base ... */
#define NEG_HI 1.12   /* ... and 90th percentile <= NEG_HI * base  -> clear / orange base (gap, leader) */
#define GAP_MIN 0.01  /* inches */
#define GAP_MAX 0.35

int pv_film_from_name(const char *s) {
  if (!s) return -1;
  if (!strcmp(s, "cpos") || !strcmp(s, "color-pos") || !strcmp(s, "colour-pos") || !strcmp(s, "slide")) return PV_CPOS;
  if (!strcmp(s, "bpos") || !strcmp(s, "bw-pos")) return PV_BPOS;
  if (!strcmp(s, "cneg") || !strcmp(s, "color-neg") || !strcmp(s, "colour-neg")) return PV_CNEG;
  if (!strcmp(s, "bneg") || !strcmp(s, "bw-neg")) return PV_BNEG;
  return -1;
}
const char *pv_film_name(int film) {
  static const char *n[] = {"cpos", "bpos", "cneg", "bneg"};
  return film >= 0 && film < 4 ? n[film] : "cpos";
}
static int is_neg(int film) { return film == PV_CNEG || film == PV_BNEG; }

static void add_cut(pv_info *pi, double c, int kind) {
  if (pi->ncuts >= PV_MAXG) return;
  for (int i = 0; i < pi->ncuts; i++) if (pi->cuts[i] > c - 0.05 && pi->cuts[i] < c + 0.05) return;
  int i = pi->ncuts++;
  while (i > 0 && pi->cuts[i - 1] > c) { pi->cuts[i] = pi->cuts[i - 1]; pi->cut_kind[i] = pi->cut_kind[i - 1]; i--; }
  pi->cuts[i] = c; pi->cut_kind[i] = kind;
}

void pv_analyze(const uint8_t *img, int w, int h, int bits, int dpi, double max_seg, int film, pv_info *pi) {
  memset(pi, 0, sizeof *pi);
  pi->dpi = dpi; pi->width = w; pi->height = h; pi->film = (film >= 0 && film < 4) ? film : PV_CPOS;
  int neg = is_neg(pi->film);
  int x0 = w / 5, x1 = w * 4 / 5, n = x1 - x0;
  if (n < 1) { x0 = 0; n = w; }
  double *v = malloc(sizeof(double) * (n > h ? n : h) + 8);
  double *p10 = malloc(sizeof(double) * h), *p90 = malloc(sizeof(double) * h), *med = malloc(sizeof(double) * h);
  uint8_t *gap = calloc(h + 1, 1);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < n; x++) v[x] = gv(img, w, bits, x0 + x, y);
    qsort(v, n, sizeof(double), cmpd);
    p10[y] = v[n / 10]; p90[y] = v[n * 9 / 10]; med[y] = v[n / 2];
  }
  int f0 = 0, f1 = h - 1;
  while (f0 < h && med[f0] >= NOFILM) f0++;
  while (f1 > f0 && med[f1] >= NOFILM) f1--;
  pi->film_start = f0 / (double)dpi; pi->film_end = f1 / (double)dpi;

  if (!neg) {
    for (int y = 0; y < h; y++) gap[y] = (y >= f0 && y <= f1 && p90[y] < DARK);
  } else {
    /* film base level: rows where even the darkest 10 % is bright are base (gaps, leader).
       Take a high percentile of the per-row p10 over the film area. */
    int m = 0;
    for (int y = f0; y <= f1; y++) if (med[y] < NOFILM) v[m++] = p10[y];
    double B = pct(v, m, 0.97);
    if (B < 0.02) B = 0.02;
    for (int y = 0; y < h; y++)
      gap[y] = (y >= f0 && y <= f1 && med[y] < NOFILM && p10[y] >= NEG_LO * B && p90[y] <= NEG_HI * B);
    /* close 1-row holes (dust, scratches) */
    for (int y = 1; y + 1 < h; y++) if (!gap[y] && gap[y - 1] && gap[y + 1]) gap[y] = 1;
    /* per-channel base from the gap rows (for the display inversion) */
    double acc[3] = {0, 0, 0}; long cnt = 0;
    for (int y = f0; y <= f1; y++) if (gap[y])
      for (int x = x0; x < x0 + n; x += 2) { for (int c = 0; c < 3; c++) acc[c] += cv(img, w, bits, x, y, c); cnt++; }
    if (cnt > 50) for (int c = 0; c < 3; c++) pi->base[c] = acc[c] / cnt;
  }

  for (int y = f0; y <= f1; ) {
    if (gap[y]) {
      int j = y; while (j <= f1 && gap[j]) j++;
      double a = y / (double)dpi, len = (j - y) / (double)dpi;
      int at_start = (y <= f0 + 2), at_end = (j >= f1 - 2);
      if (len >= GAP_MIN && len <= GAP_MAX && !at_start && !at_end) {
        if (pi->ngaps < PV_MAXG) { pi->gap_c[pi->ngaps] = a + len / 2; pi->gap_w[pi->ngaps] = len; pi->ngaps++; }
        add_cut(pi, a + len / 2, 0);
      } else if (len > GAP_MAX) {                       /* long gap-like run (leader / blank frame) */
        if (!at_start) add_cut(pi, a + 0.04, 1);
        if (!at_end)   add_cut(pi, a + len - 0.04, 1);
      }
      y = j;
    } else y++;
  }
  double maxlen = h / (double)dpi;
  pi->end = pi->film_end + 0.05 < maxlen ? pi->film_end + 0.05 : maxlen;
  pi->start = pi->film_start - 0.05 > 0 ? pi->film_start - 0.05 : 0;
  for (int i = 0; i < pi->ncuts; ) {          /* cuts outside [start, end] are meaningless */
    if (pi->cuts[i] <= pi->start + 0.05 || pi->cuts[i] >= pi->end - 0.05) {
      memmove(pi->cuts + i, pi->cuts + i + 1, sizeof(double) * (pi->ncuts - i - 1));
      memmove(pi->cut_kind + i, pi->cut_kind + i + 1, sizeof(int) * (pi->ncuts - i - 1)); pi->ncuts--;
    } else i++;
  }
  /* drop leader-edge cuts that would leave a sliver (< 0.5") */
  for (int i = 0; i < pi->ncuts; ) {
    double a = i ? pi->cuts[i - 1] : pi->start, b = i + 1 < pi->ncuts ? pi->cuts[i + 1] : pi->end;
    if (pi->cut_kind[i] == 1 && (pi->cuts[i] - a < 0.5 || b - pi->cuts[i] < 0.5)) {
      memmove(pi->cuts + i, pi->cuts + i + 1, sizeof(double) * (pi->ncuts - i - 1));
      memmove(pi->cut_kind + i, pi->cut_kind + i + 1, sizeof(int) * (pi->ncuts - i - 1));
      pi->ncuts--;
    } else i++;
  }
  /* forced splits for over-long segments (e.g. a long leader), but ONLY inside gap-like rows so a
     split never cuts through a picture. A long segment with no such place is left as is
     (its window is just longer; the CLI refuses > 4.5"). */
  if (max_seg > 0) {
    for (int pass = 0; pass < 8; pass++) {
      int added = 0;
      double prev = pi->start;
      int nseg = pi->ncuts + 1;
      double sa[PV_MAXG + 1], sb[PV_MAXG + 1];
      for (int i = 0; i < nseg; i++) { sa[i] = prev; sb[i] = i < pi->ncuts ? pi->cuts[i] : pi->end; prev = sb[i]; }
      for (int i = 0; i < nseg; i++) {
        double len = sb[i] - sa[i];
        if (len <= max_seg) continue;
        double target = sa[i] + len / 2, best = -1, bd = 1e9;
        for (int y = (int)((sa[i] + 0.4) * dpi); y < (int)((sb[i] - 0.4) * dpi) && y < h; y++) {
          if (y < 0 || !gap[y]) continue;
          double d = fabs(y / (double)dpi - target);
          if (d < bd) { bd = d; best = y / (double)dpi; }
        }
        if (best > 0) { add_cut(pi, best, 2); added = 1; }
      }
      if (!added) break;
    }
  }
  pi->p90 = malloc(sizeof(float) * h); pi->np = h;
  if (pi->p90) for (int y = 0; y < h; y++) pi->p90[y] = (float)p90[y];
  pi->gaprow = gap;
  free(v); free(p10); free(p90); free(med);
}
void pv_free(pv_info *pi) { free(pi->p90); pi->p90 = NULL; free(pi->gaprow); pi->gaprow = NULL; pi->np = 0; }

void pv_print(const pv_info *pi) {
  printf("film type: %s\n", pv_film_name(pi->film));
  printf("film: %.3f\" .. %.3f\" (from strip window top)\n", pi->film_start, pi->film_end);
  printf("gaps (centre, width):");
  for (int i = 0; i < pi->ngaps; i++) printf(" %.3f(%.3f)", pi->gap_c[i], pi->gap_w[i]);
  printf("\nsuggested --cuts ");
  for (int i = 0; i < pi->ncuts; i++) printf("%s%.3f", i ? "," : "", pi->cuts[i]);
  printf(" --start %.3f --end %.3f\n", pi->start, pi->end);
  for (int i = 0; i < pi->ncuts; i++)
    if (pi->cut_kind[i] == 2) printf("note: cut %.3f is a forced split (segment too long), not in a gap\n", pi->cuts[i]);
}

int pv_write_json(const char *path, const pv_info *pi, const char *tiff, const char *png) {
  FILE *f = fopen(path, "w"); if (!f) return -1;
  fprintf(f, "{\n  \"dpi\": %d, \"width\": %d, \"height\": %d,\n  \"tiff\": \"%s\", \"png\": \"%s\",\n",
          pi->dpi, pi->width, pi->height, tiff, png);
  fprintf(f, "  \"film_start\": %.4f, \"film_end\": %.4f, \"start\": %.4f, \"end\": %.4f,\n  \"gaps\": [", pi->film_start, pi->film_end, pi->start, pi->end);
  for (int i = 0; i < pi->ngaps; i++) fprintf(f, "%s{\"c\": %.4f, \"w\": %.4f}", i ? ", " : "", pi->gap_c[i], pi->gap_w[i]);
  fprintf(f, "],\n  \"cuts\": [");
  for (int i = 0; i < pi->ncuts; i++) fprintf(f, "%s%.4f", i ? ", " : "", pi->cuts[i]);
  fprintf(f, "],\n  \"cut_kind\": [");
  for (int i = 0; i < pi->ncuts; i++) fprintf(f, "%s%d", i ? ", " : "", pi->cut_kind[i]);
  fprintf(f, "],\n  \"film\": \"%s\", \"base\": [%.4f, %.4f, %.4f],\n  \"gaprow\": [",
          pv_film_name(pi->film), pi->base[0], pi->base[1], pi->base[2]);
  for (int i = 0; pi->gaprow && i < pi->np; i++) fprintf(f, "%s%d", i ? "," : "", pi->gaprow[i]);
  fprintf(f, "],\n  \"p90\": [");
  for (int i = 0; pi->p90 && i < pi->np; i++) fprintf(f, "%s%.3f", i ? "," : "", pi->p90[i]);
  fprintf(f, "]\n}\n");
  return fclose(f);
}

/* ---- PNG (8-bit RGB, global auto-levels 0.5..99.5 % on green of the film area) ---- */
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
  uint8_t hdr[8]; be32(hdr, n); memcpy(hdr + 4, type, 4);
  uint32_t c = crc32(0, (const Bytef *)type, 4); if (n) c = crc32(c, d, n);
  uint8_t cr[4]; be32(cr, c);
  return fwrite(hdr, 1, 8, f) != 8 || (n && fwrite(d, 1, n, f) != n) || fwrite(cr, 1, 4, f) != 4;
}
static int write_png_rgb8(const char *path, const uint8_t *raw, size_t raw_n, int w, int h) {
  uLongf zn = compressBound(raw_n);
  uint8_t *z = malloc(zn);
  if (!z || compress2(z, &zn, raw, raw_n, 6) != Z_OK) { free(z); return -1; }
  FILE *f = fopen(path, "wb");
  if (!f) { free(z); return -1; }
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  uint8_t ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
  int bad = fwrite(sig, 1, 8, f) != 8 || chunk(f, "IHDR", ihdr, 13) || chunk(f, "IDAT", z, (uint32_t)zn) || chunk(f, "IEND", NULL, 0);
  bad |= fclose(f) != 0;
  free(z);
  return bad ? -1 : 0;
}

/* negatives, display only: t = raw / base (removes the orange mask for colour negs), density
   d = -log10(t), then per-channel auto-levels (0.5 .. 99.5 %) over the film area. B&W: green only. */
static int write_png_neg(const char *path, const uint8_t *img, int w, int h, int bits, int film, const pv_info *pi) {
  int bw = film == PV_BNEG;
  double base[3];
  if (pi && pi->base[1] > 0) for (int c = 0; c < 3; c++) base[c] = pi->base[c] > 0.005 ? pi->base[c] : 0.005;
  else {                                         /* estimate: 99th percentile per channel below NOFILM */
    for (int c = 0; c < 3; c++) {
      int hist[1024] = {0}; long tot = 0;
      for (int y = 0; y < h; y += 3) for (int x = 0; x < w; x += 3) {
        if (gv(img, w, bits, x, y) >= NOFILM) continue;
        double q = cv(img, w, bits, x, y, c); hist[(int)(q * 1023)]++; tot++;
      }
      long acc = 0; base[c] = 1;
      for (int i = 0; i < 1024; i++) { acc += hist[i]; if (acc >= tot * 0.99) { base[c] = (i + 1) / 1023.0; break; } }
      if (base[c] < 0.005) base[c] = 0.005;
    }
  }
  int f0 = 0, f1 = h - 1;
  if (pi) { f0 = (int)(pi->film_start * pi->dpi); f1 = (int)(pi->film_end * pi->dpi); if (f1 <= f0) { f0 = 0; f1 = h - 1; } }
  const double DMAX = 3.0;
  double lo[3], hi[3];
  for (int c = 0; c < 3; c++) {
    int cc = bw ? 1 : c;
    static int hist[4096]; memset(hist, 0, sizeof hist); long tot = 0;
    for (int y = f0; y <= f1; y += 2) for (int x = 0; x < w; x += 2) {
      double t = cv(img, w, bits, x, y, cc) / base[cc];
      t = t > 1e-4 ? t : 1e-4; double d = -log10(t); if (d < 0) d = 0; if (d > DMAX) d = DMAX;
      hist[(int)(d / DMAX * 4095)]++; tot++;
    }
    long acc = 0; int got = 0; lo[c] = 0; hi[c] = DMAX;
    for (int i = 0; i < 4096; i++) {
      acc += hist[i];
      if (!got && acc >= tot * 0.005) { lo[c] = i / 4095.0 * DMAX; got = 1; }
      if (acc >= tot * 0.995) { hi[c] = (i + 1) / 4095.0 * DMAX; break; }
    }
    if (hi[c] - lo[c] < 0.05) hi[c] = lo[c] + 0.05;
  }
  size_t rb = (size_t)w * 3 + 1, raw_n = rb * h;
  uint8_t *raw = malloc(raw_n);
  if (!raw) return -1;
  for (int y = 0; y < h; y++) {
    uint8_t *r = raw + rb * y; r[0] = 0;
    for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
      int cc = bw ? 1 : c;
      double t = cv(img, w, bits, x, y, cc) / base[cc];
      t = t > 1e-4 ? t : 1e-4; double d = -log10(t);
      double q = (d - lo[c]) / (hi[c] - lo[c]); if (q < 0) q = 0; if (q > 1) q = 1;
      r[1 + x * 3 + c] = (uint8_t)(q * 255 + 0.5);
    }
  }
  int rc = write_png_rgb8(path, raw, raw_n, w, h);
  free(raw); return rc;
}

int pv_write_png(const char *path, const uint8_t *img, int w, int h, int bits, int film, const pv_info *pi) {
  if (is_neg(film)) return write_png_neg(path, img, w, h, bits, film, pi);
  int hist[4096] = {0}; long tot = 0;
  for (int y = 0; y < h; y += 2) for (int x = 0; x < w; x += 2) {
    double g = gv(img, w, bits, x, y);
    if (g < 0.98) { hist[(int)(g * 4095)]++; tot++; }
  }
  double lo = 0, hi = 1; long acc = 0; int got_lo = 0;
  for (int i = 0; i < 4096; i++) {
    acc += hist[i];
    if (!got_lo && acc >= tot * 0.005) { lo = i / 4095.0; got_lo = 1; }
    if (acc >= tot * 0.995) { hi = (i + 1) / 4095.0; break; }
  }
  if (hi - lo < 0.02) hi = lo + 0.02;
  int bw = film == PV_BPOS;
  size_t rb = (size_t)w * 3 + 1, raw_n = rb * h;
  uint8_t *raw = malloc(raw_n);
  if (!raw) return -1;
  for (int y = 0; y < h; y++) {
    uint8_t *r = raw + rb * y; r[0] = 0;
    for (int x = 0; x < w * 3; x++) {
      double v = bw ? gv(img, w, bits, x / 3, y) : cv(img, w, bits, x / 3, y, x % 3);
      v = (v - lo) / (hi - lo); if (v < 0) v = 0; if (v > 1) v = 1;
      r[1 + x] = (uint8_t)(v * 255 + 0.5);
    }
  }
  int rc = write_png_rgb8(path, raw, raw_n, w, h);
  free(raw); return rc;
}
