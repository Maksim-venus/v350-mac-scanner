#ifndef V350PREVIEW_H
#define V350PREVIEW_H
#include <stdint.h>
#define PV_MAXG 64
typedef struct {
  int dpi, width, height;
  double film_start, film_end;          /* inches from window top */
  int ngaps; double gap_c[PV_MAXG], gap_w[PV_MAXG];
  int ncuts; double cuts[PV_MAXG];      /* suggested cuts (gaps + leader edges + splits) */
  int cut_kind[PV_MAXG];                /* 0 gap, 1 leader edge, 2 forced split */
  double start, end;                    /* suggested scan start / end */
  float *p90; int np;                   /* per-row 90th percentile (0..1) of green */
  uint8_t *gaprow;                      /* per-row 1 = gap-like (no picture content), for blank detection */
  int film;                             /* PV_* film type used for detection */
  double base[3];                       /* negatives: film-base level per channel (0..1), 0 if unknown */
} pv_info;

/* film types: detection differs (positives: gaps are dark; negatives: gaps are clear/orange base) */
enum { PV_CPOS = 0, PV_BPOS = 1, PV_CNEG = 2, PV_BNEG = 3 };
int pv_film_from_name(const char *s);   /* "cpos" "bpos" "cneg" "bneg" (also colour-pos ...), -1 if bad */
const char *pv_film_name(int film);

/* img: interleaved RGB, 8 or 16 bit (LE), w x h at dpi */
void pv_analyze(const uint8_t *img, int w, int h, int bits, int dpi, double max_seg, int film, pv_info *pi);
/* display PNG: positives auto-levelled; negatives inverted (colour: base/orange mask removed).
   Display only - scanned TIFFs are always raw. pi may be NULL (then base is estimated). */
int  pv_write_png(const char *path, const uint8_t *img, int w, int h, int bits, int film, const pv_info *pi);
int  pv_write_json(const char *path, const pv_info *pi, const char *tiff, const char *png);
void pv_print(const pv_info *pi);
void pv_free(pv_info *pi);
#endif
