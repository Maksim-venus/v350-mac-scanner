/* v350stitch.c — cut-based alignment and joining of AFL windows, streamed to TIFF */
#include "v350stitch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int stitch_init(v350_stitch *st, v350_tiff_stream *out, int width, int channels, int bits,
                int dpi, int nseg, int m, int verbose) {
  memset(st, 0, sizeof *st);
  st->out = out; st->width = width; st->channels = channels; st->bits = bits; st->dpi = dpi;
  st->row_bytes = (size_t)width * channels * (bits / 8);
  st->nseg = nseg; st->verbose = verbose; st->m = m;
  st->A = m / 2;                       /* match band: cut +- m/2 (gap + content both sides) */
  st->S = m * 2 / 5;                   /* dy search:  +- 0.4 m                              */
  st->DX = dpi * 15 / 1000;            /* dx search:  +- 0.015"                             */
  if (st->DX < 4) st->DX = 4;
  st->snapR = m / 4;                   /* snap join to darkest row within +- m/4            */
  if (st->A + st->S >= m) st->S = m - st->A - 1;
  if (nseg > 1) {
    st->tail = malloc((size_t)2 * m * st->row_bytes);
    st->head = malloc((size_t)2 * m * st->row_bytes);
    if (!st->tail || !st->head) return -1;
  }
  return 0;
}

int stitch_begin_segment(v350_stitch *st, int seg, int exp_h) {
  st->seg = seg; st->exp_h = exp_h; st->last = (seg == st->nseg - 1);
  if (!st->last && exp_h < 4 * st->m) return -1;
  if (seg > 0 && exp_h < 2 * st->m) return -1;
  st->hold_from = st->last ? exp_h + 1 : exp_h - 2 * st->m;
  st->head_n = 0; st->head_need = 2 * st->m;
  st->seam_done = (seg == 0);
  return 0;
}

static inline float gpx(const v350_stitch *st, const uint8_t *row, int x) {
  int c = st->channels == 3 ? 1 : 0;
  if (st->bits == 8) return row[(size_t)x * st->channels + c] / 255.0f;
  const uint8_t *p = row + ((size_t)x * st->channels + c) * 2;
  return (p[0] | (p[1] << 8)) / 65535.0f;
}

/* green plane, optionally box-decimated by g */
static float *plane(const v350_stitch *st, const uint8_t *rows, int n, int g, int *ow, int *oh) {
  int W = st->width / g, H = n / g;
  float *p = calloc((size_t)W * H + 1, sizeof(float));
  if (!p) return NULL;
  for (int y = 0; y < H * g; y++) {
    const uint8_t *r = rows + (size_t)y * st->row_bytes;
    float *d = p + (size_t)(y / g) * W;
    for (int x = 0; x < W * g; x++) d[x / g] += gpx(st, r, x);
  }
  float k = 1.0f / (g * g);
  for (size_t i = 0; i < (size_t)W * H; i++) p[i] *= k;
  *ow = W; *oh = H;
  return p;
}

/* NCC between head rows [j0,j1) and tail rows [j0+s, j1+s), head x vs tail x+dx */
static double ncc(const float *h, const float *t, int W, int H, int j0, int j1, int s, int dx,
                  int rs, int cs, double *hstd) {
  int x0 = dx < 0 ? -dx : 0, x1 = dx > 0 ? W - dx : W;
  if (j0 + s < 0 || j1 + s > H || x1 - x0 < 16) return -2;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; long n = 0;
  for (int j = j0; j < j1; j += rs) {
    const float *a = h + (size_t)j * W, *b = t + (size_t)(j + s) * W + dx;
    for (int x = x0; x < x1; x += cs) {
      double u = a[x], v = b[x];
      sa += u; sb += v; saa += u * u; sbb += v * v; sab += u * v; n++;
    }
  }
  if (n < 2) return -2;
  double va = saa - sa * sa / n, vb = sbb - sb * sb / n, cv = sab - sa * sb / n;
  if (hstd) *hstd = sqrt(va > 0 ? va / n : 0);
  if (va <= 1e-12 || vb <= 1e-12) return 0;
  return cv / sqrt(va * vb);
}

static int resolve_seam(v350_stitch *st) {
  int k = st->seg < STITCH_MAXSEG ? st->seg : STITCH_MAXSEG - 1;
  int m = st->m, n2 = 2 * m;
  int g = st->dpi / 300; if (g < 1) g = 1;
  int bs = 0, bdx = 0; double best = -2, hstd = 0;
  int Wc, Hc, Wf, Hf;
  float *hc = plane(st, st->head, n2, g, &Wc, &Hc), *tc = plane(st, st->tail, n2, g, &Wc, &Hc);
  float *hf = g > 1 ? plane(st, st->head, n2, 1, &Wf, &Hf) : hc;
  float *tf = g > 1 ? plane(st, st->tail, n2, 1, &Wf, &Hf) : tc;
  if (!hc || !tc || !hf || !tf) { fprintf(stderr, "stitch: out of memory\n"); return -1; }
  if (g == 1) { Wf = Wc; Hf = Hc; }
  /* coarse */
  int Sc = st->S / g, DXc = st->DX / g;
  int j0c = (m - st->A) / g, j1c = (m + st->A) / g;
  for (int s = -Sc; s <= Sc; s++)
    for (int dx = -DXc; dx <= DXc; dx++) {
      double c = ncc(hc, tc, Wc, Hc, j0c, j1c, s, dx, 1, 1, NULL);
      if (c > best) { best = c; bs = s * g; bdx = dx * g; }
    }
  /* refine at full resolution */
  if (g > 1) {
    int rs = g / 4 > 0 ? g / 4 : 1, cs = g / 2 > 0 ? g / 2 : 1;
    int cs0 = bs, cdx0 = bdx; best = -2;
    for (int s = cs0 - g; s <= cs0 + g; s++) {
      if (s < -st->S || s > st->S) continue;
      for (int dx = cdx0 - g; dx <= cdx0 + g; dx++) {
        double c = ncc(hf, tf, Wf, Hf, m - st->A, m + st->A, s, dx, rs, cs, NULL);
        if (c > best) { best = c; bs = s; bdx = dx; }
      }
    }
  }
  ncc(hf, tf, Wf, Hf, m - st->A, m + st->A, 0, 0, 1, 1, &hstd);
  st->flat[k] = 0;
  double e_prev = st->e_cur;
  if (hstd < 0.004 || best < 0.3) {          /* no usable texture: trust the (bias-corrected) plan */
    bs = -(int)lround(e_prev); if (bs > st->S) bs = st->S; if (bs < -st->S) bs = -st->S;
    bdx = 0; st->flat[k] = 1;
  }
  st->e_cur = e_prev + bs;
  /* cut position in previous window's tail, snapped to the darkest band */
  int Jt0 = m - (int)lround(e_prev), Jt = Jt0, snapped = 0;
  {
    int x0 = Wf / 5, x1 = Wf * 4 / 5, w = st->dpi / 100; if (w < 1) w = 1;
    double bestv = 1e9; int bi = Jt0;
    for (int i = Jt0 - st->snapR; i <= Jt0 + st->snapR; i++) {
      if (i - w < 0 || i + w >= n2) continue;
      double sum = 0; int n = 0;
      for (int r = i - w; r <= i + w; r++) {
        const float *row = tf + (size_t)r * Wf;
        for (int x = x0; x < x1; x += 2) { sum += row[x]; n++; }
      }
      double v = sum / n;
      if (v < bestv - 1e-6 || (fabs(v - bestv) <= 1e-6 && abs(i - Jt0) < abs(bi - Jt0))) { bestv = v; bi = i; }
    }
    if (bestv < 0.05) { Jt = bi; snapped = 1; }
  }
  int jh = Jt - bs;                       /* first head line written */
  if (Jt < 1) Jt = 1;
  if (Jt > n2) Jt = n2;
  if (jh < 0) jh = 0;
  if (jh > st->head_n) jh = st->head_n;
  st->shift[k] = bs; st->dx[k] = bdx; st->ncc[k] = best; st->snap[k] = Jt - Jt0; st->err[k] = st->e_cur;
  fprintf(stderr, "seam %d/%d: dy %+d lines (%+.4f\"), dx %+d px, ncc %.3f%s, join %s%+d lines from cut, window error now %+.4f\"\n",
          st->seg, st->seg + 1, bs, bs / (double)st->dpi, bdx, best, st->flat[k] ? " (flat -> nominal)" : "",
          snapped ? "snapped to gap " : "", Jt - Jt0, st->e_cur / st->dpi);
  if (g > 1) { free(hf); free(tf); }
  free(hc); free(tc);
  if (v350_tiff_write_rows(st->out, st->tail, Jt)) return -1;
  if (st->head_n > jh &&
      v350_tiff_write_rows(st->out, st->head + (size_t)jh * st->row_bytes, st->head_n - jh)) return -1;
  st->tail_n = 0;
  st->seam_done = 1;
  return 0;
}

int stitch_line(void *ctx, const uint8_t *line, size_t bpl, int index) {
  v350_stitch *st = ctx;
  if (bpl != st->row_bytes) { fprintf(stderr, "stitch: row size %zu != %zu\n", bpl, st->row_bytes); return -1; }
  if (!st->seam_done) {
    memcpy(st->head + (size_t)st->head_n * st->row_bytes, line, bpl);
    if (++st->head_n == st->head_need) return resolve_seam(st);
    return 0;
  }
  if (index >= st->hold_from) {
    int t = index - st->hold_from;
    if (t >= 2 * st->m) return -1;
    memcpy(st->tail + (size_t)t * st->row_bytes, line, bpl);
    st->tail_n = t + 1;
    return 0;
  }
  return v350_tiff_write_rows(st->out, line, 1);
}

int stitch_end_segment(v350_stitch *st, int got_h) {
  if (got_h != st->exp_h) { fprintf(stderr, "stitch: window %d got %d lines, expected %d\n", st->seg + 1, got_h, st->exp_h); return -1; }
  if (!st->seam_done) return -1;
  if (!st->last && st->tail_n != 2 * st->m) return -1;
  return 0;
}

void stitch_free(v350_stitch *st) {
  if (st->out) {
    /* partial scan: flush whatever is held so the partial TIFF is usable */
    if (st->tail_n > 0) v350_tiff_write_rows(st->out, st->tail, st->tail_n);
    if (!st->seam_done && st->head_n > 0) v350_tiff_write_rows(st->out, st->head, st->head_n);
  }
  free(st->tail); free(st->head);
  st->tail = st->head = NULL; st->tail_n = 0;
}
