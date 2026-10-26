/* v350film — "finished" positives from negative scans (modelled on a scanner driver's negative
   pipeline: film-base removal, inversion, per-channel auto levels, display gamma).
   Parameters are computed once per strip from the 300 dpi preview, so all frames match. */
#ifndef V350FILM_H
#define V350FILM_H
#include <stdint.h>

typedef struct {
  int valid;
  int bw;                 /* 1 = B&W negative (grey output) */
  double base[3];         /* film base (orange mask) raw level per channel, 0..1 linear */
  double dlo[3], dhi[3];  /* density above base mapped to black (dlo) and white (dhi) */
  double mid[3];          /* per-channel mid-tone exponent (grey balance; 1 = none) */
  double S;               /* log10 scene range the [dlo,dhi] window is expanded to (contrast) */
  double gamma;           /* output encoding gamma (display target) */
} v350_finish;

#define FIN_S_DEFAULT     2.2
#define FIN_GAMMA_DEFAULT 2.2
#define FIN_CLIP_SHADOW   0.001   /* fraction of image pixels allowed to clip to black */
#define FIN_CLIP_HIGH     0.005   /* ... and to white */
#define FIN_MID_LIMIT     1.30    /* grey balance: mid-tone exponent kept within [1/x, x] */

/* img: interleaved RGB 8/16-bit LE; mask (h rows) 1 = row may be used for statistics;
   x0..x1 = image columns to use; base: from gap rows (NULL/zeros = estimate) */
int  fin_compute(v350_finish *f, const uint8_t *img, int w, int h, int bits, const uint8_t *rowmask,
                 int x0, int x1, const double *base, int bw);
/* output value 0..1 for input linear raw v (0..1) of channel c */
double fin_eval(const v350_finish *f, int c, double v);

typedef struct { int bits, chan_out; uint16_t *lut[3]; int n; } fin_lut;
int  fin_lut_build(fin_lut *L, const v350_finish *f, int bits);
void fin_lut_free(fin_lut *L);
/* convert one row of w RGB pixels into L->chan_out channels (same bit depth) */
void fin_row(const fin_lut *L, const uint8_t *in, uint8_t *out, int w);

int  fin_to_json(const v350_finish *f, char *buf, int n);   /* "finish": {...} body */
int  fin_from_json(v350_finish *f, const char *json_text);  /* parses "finish": {...} */
#endif
