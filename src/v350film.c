/* v350film.c — see v350film.h.
 *
 * Model (per channel c, raw value v = linear transmittance as delivered by the scanner):
 *   t = v / base_c                 remove the film base / orange mask
 *   d = -log10(t)                  density above base (0 = unexposed, larger = brighter scene)
 *   q = (d - dlo_c)/(dhi_c - dlo_c) per-channel black / white point (auto levels with small
 *                                  clipping)
 *   q = q^mid_c                    mid-tone grey balance: the median of R and B is matched to G
 *                                  (grey-world, limited to a gentle correction)
 *   L = (10^(-S (1-q)) - 10^-S) / (1 - 10^-S)    back to linear scene light over S decades
 *   out = L^(1/gamma)              display encoding (gamma 2.2)
 * B&W: the three channels are averaged first and channel 1's parameters are used; grey output.
 */
#include "v350film.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline double px(const uint8_t *img, int w, int bits, int x, int y, int c) {
  size_t i = ((size_t)y * w + x) * 3 + c;
  return bits == 8 ? img[i] / 255.0 : (img[2 * i] | (img[2 * i + 1] << 8)) / 65535.0;
}

/* statistics use 3x3 box means: at 8 bits a dense negative's blue channel has only a few raw
   levels, and averaging noisy neighbours recovers sub-level precision */
static double px3(const uint8_t *img, int w, int h, int bits, int x, int y, int c) {
  double s = 0; int n = 0;
  for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
    int xx = x + dx, yy = y + dy;
    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
    s += px(img, w, bits, xx, yy, c); n++;
  }
  return s / n;
}
#define NB 4096
#define DMAX 3.5
int fin_compute(v350_finish *f, const uint8_t *img, int w, int h, int bits, const uint8_t *rowmask,
                int x0, int x1, const double *base, int bw) {
  memset(f, 0, sizeof *f);
  f->bw = bw; f->S = FIN_S_DEFAULT; f->gamma = FIN_GAMMA_DEFAULT;
  if (x0 < 0) x0 = 0;
  if (x1 > w) x1 = w;
  if (x1 <= x0) return -1;
  int step = (int)(((size_t)(x1 - x0) * h) / 400000) + 1;     /* ~400k samples are plenty */
  int sx = step > 4 ? 4 : step, sy = (step + sx - 1) / sx;
  /* base: from the gap rows if given, else the brightest robust (99.5 %) value of the film area.
     Nothing on a negative is brighter than unexposed base, so a "base" well below that
     percentile means the gap rows were wrong (then the percentile is used). */
  for (int c = 0; c < 3; c++) {
    static int hist[NB]; memset(hist, 0, sizeof hist); long tot = 0;
    for (int y = 0; y < h; y += sy) {
      for (int x = x0; x < x1; x += sx) {
        if (px(img, w, bits, x, y, 1) >= 0.9) continue;     /* lamp (no film) */
        double v = px3(img, w, h, bits, x, y, c);
        hist[(int)(v * (NB - 1))]++; tot++;
      }
    }
    long acc = 0; double P = 1;
    for (int i = 0; i < NB; i++) { acc += hist[i]; if (acc >= tot * 0.995) { P = (i + 1) / (double)(NB - 1); break; } }
    f->base[c] = (base && base[c] > 0.002 && base[c] >= 0.85 * P) ? base[c] : P;
  }
  if (bw) { double m = (f->base[0] + f->base[1] + f->base[2]) / 3; f->base[0] = f->base[1] = f->base[2] = m; }
  for (int c = 0; c < 3; c++) if (f->base[c] < 0.002) f->base[c] = 0.002;
  /* density histograms over image pixels */
  for (int c = 0; c < 3; c++) {
    if (bw && c != 1) continue;
    static int hist[NB]; memset(hist, 0, sizeof hist); long tot = 0;
    for (int y = 0; y < h; y += sy) {
      if (rowmask && !rowmask[y]) continue;
      for (int x = x0; x < x1; x += sx) {
        double v = bw ? (px3(img, w, h, bits, x, y, 0) + px3(img, w, h, bits, x, y, 1) + px3(img, w, h, bits, x, y, 2)) / 3
                      : px3(img, w, h, bits, x, y, c);
        double t = v / f->base[c]; if (t < 1e-5) t = 1e-5;
        double d = -log10(t); if (d < 0) d = 0; if (d > DMAX) d = DMAX;
        hist[(int)(d / DMAX * (NB - 1))]++; tot++;
      }
    }
    if (tot < 100) return -1;
    long acc = 0; int got = 0; f->dlo[c] = 0; f->dhi[c] = DMAX;
    for (int i = 0; i < NB; i++) {
      acc += hist[i];
      if (!got && acc > tot * FIN_CLIP_SHADOW) { f->dlo[c] = i / (double)(NB - 1) * DMAX; got = 1; }
      if (acc >= tot * (1 - FIN_CLIP_HIGH)) { f->dhi[c] = (i + 1) / (double)(NB - 1) * DMAX; break; }
    }
    if (f->dhi[c] - f->dlo[c] < 0.1) f->dhi[c] = f->dlo[c] + 0.1;
  }
  if (bw) for (int c = 0; c < 3; c += 2) { f->dlo[c] = f->dlo[1]; f->dhi[c] = f->dhi[1]; }
  /* grey balance: per-channel medians of the levelled values */
  double med[3] = {0.5, 0.5, 0.5};
  for (int c = 0; c < 3; c++) f->mid[c] = 1;
  if (!bw) {
    for (int c = 0; c < 3; c++) {
      static int hist[NB]; memset(hist, 0, sizeof hist); long tot = 0;
      for (int y = 0; y < h; y += sy) {
        if (rowmask && !rowmask[y]) continue;
        for (int x = x0; x < x1; x += sx) {
          double t = px3(img, w, h, bits, x, y, c) / f->base[c]; if (t < 1e-5) t = 1e-5;
          double q = (-log10(t) - f->dlo[c]) / (f->dhi[c] - f->dlo[c]);
          q = q < 0 ? 0 : q > 1 ? 1 : q;
          hist[(int)(q * (NB - 1))]++; tot++;
        }
      }
      long acc = 0;
      for (int i = 0; i < NB; i++) { acc += hist[i]; if (acc >= tot / 2) { med[c] = (i + 0.5) / (NB - 1); break; } }
      if (med[c] < 0.02) med[c] = 0.02;
      if (med[c] > 0.98) med[c] = 0.98;
    }
    for (int c = 0; c < 3; c += 2) {
      double k = log(med[1]) / log(med[c]);
      if (k > FIN_MID_LIMIT) k = FIN_MID_LIMIT;
      if (k < 1 / FIN_MID_LIMIT) k = 1 / FIN_MID_LIMIT;
      f->mid[c] = k;
    }
  }
  f->valid = 1;
  return 0;
}

double fin_eval(const v350_finish *f, int c, double v) {
  double t = v / f->base[c]; if (t < 1e-6) t = 1e-6;
  double d = -log10(t);
  double q = (d - f->dlo[c]) / (f->dhi[c] - f->dlo[c]);
  q = q < 0 ? 0 : q > 1 ? 1 : q;
  if (f->mid[c] > 0 && f->mid[c] != 1) q = pow(q, f->mid[c]);
  double floor_ = pow(10, -f->S);
  double L = (pow(10, -f->S * (1 - q)) - floor_) / (1 - floor_);
  if (L < 0) L = 0;
  return pow(L, 1.0 / f->gamma);
}

int fin_lut_build(fin_lut *L, const v350_finish *f, int bits) {
  memset(L, 0, sizeof *L);
  L->bits = bits; L->chan_out = f->bw ? 1 : 3; L->n = bits == 8 ? 256 : 65536;
  double mx = L->n - 1;
  for (int c = 0; c < 3; c++) {
    L->lut[c] = malloc(sizeof(uint16_t) * L->n);
    if (!L->lut[c]) { fin_lut_free(L); return -1; }
    for (int i = 0; i < L->n; i++) L->lut[c][i] = (uint16_t)lround(fin_eval(f, c, i / mx) * mx);
  }
  return 0;
}
void fin_lut_free(fin_lut *L) { for (int c = 0; c < 3; c++) { free(L->lut[c]); L->lut[c] = NULL; } }

void fin_row(const fin_lut *L, const uint8_t *in, uint8_t *out, int w) {
  if (L->bits == 8) {
    for (int x = 0; x < w; x++) {
      const uint8_t *p = in + (size_t)x * 3;
      if (L->chan_out == 1) out[x] = (uint8_t)L->lut[1][(p[0] + p[1] + p[2] + 1) / 3];
      else for (int c = 0; c < 3; c++) out[(size_t)x * 3 + c] = (uint8_t)L->lut[c][p[c]];
    }
  } else {
    const uint8_t *p = in;
    for (int x = 0; x < w; x++, p += 6) {
      unsigned r = p[0] | p[1] << 8, g = p[2] | p[3] << 8, b = p[4] | p[5] << 8;
      if (L->chan_out == 1) { uint16_t v = L->lut[1][(r + g + b + 1) / 3]; out[2 * x] = v & 255; out[2 * x + 1] = v >> 8; }
      else {
        uint16_t o[3] = {L->lut[0][r], L->lut[1][g], L->lut[2][b]};
        for (int c = 0; c < 3; c++) { out[(size_t)x * 6 + 2 * c] = o[c] & 255; out[(size_t)x * 6 + 2 * c + 1] = o[c] >> 8; }
      }
    }
  }
}

int fin_to_json(const v350_finish *f, char *buf, int n) {
  return snprintf(buf, n, "{\"valid\": %d, \"bw\": %d, \"base\": [%.5f, %.5f, %.5f], \"dlo\": [%.4f, %.4f, %.4f], "
                  "\"dhi\": [%.4f, %.4f, %.4f], \"mid\": [%.4f, %.4f, %.4f], \"S\": %.3f, \"gamma\": %.3f}",
                  f->valid, f->bw, f->base[0], f->base[1], f->base[2], f->dlo[0], f->dlo[1], f->dlo[2],
                  f->dhi[0], f->dhi[1], f->dhi[2], f->mid[0], f->mid[1], f->mid[2], f->S, f->gamma);
}
static int arr3(const char *s, const char *key, double *v) {
  const char *p = strstr(s, key); if (!p || !(p = strchr(p, '['))) return -1;
  for (int i = 0; i < 3; i++) { char *e; v[i] = strtod(p + 1, &e); if (e == p + 1) return -1; p = strchr(e, i < 2 ? ',' : ']'); if (!p) return -1; }
  return 0;
}
static int num(const char *s, const char *key, double *v) {
  const char *p = strstr(s, key); if (!p || !(p = strchr(p, ':'))) return -1;
  *v = strtod(p + 1, NULL); return 0;
}
int fin_from_json(v350_finish *f, const char *text) {
  memset(f, 0, sizeof *f);
  const char *s = strstr(text, "\"finish\""); if (!s) return -1;
  double v;
  if (num(s, "\"valid\"", &v) || v < 1) return -1;
  if (num(s, "\"bw\"", &v)) return -1;
  f->bw = (int)v;
  if (arr3(s, "\"base\"", f->base) || arr3(s, "\"dlo\"", f->dlo) || arr3(s, "\"dhi\"", f->dhi)) return -1;
  if (arr3(s, "\"mid\"", f->mid)) f->mid[0] = f->mid[1] = f->mid[2] = 1;
  if (num(s, "\"S\"", &f->S) || num(s, "\"gamma\"", &f->gamma)) return -1;
  for (int c = 0; c < 3; c++) if (f->base[c] <= 0 || f->dhi[c] <= f->dlo[c]) return -1;
  f->valid = 1; return 0;
}
