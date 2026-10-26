#ifndef V350STITCH_H
#define V350STITCH_H
#include <stdint.h>
#include <stddef.h>
#include "v350lib.h"

/* Cut-based stitching of AFL windows into one streamed TIFF.
   Window k covers [cut_k - m, cut_{k+1} + m] (true inches). Consecutive windows
   overlap by 2m around each cut. The overlap is aligned by 2-D normalised
   cross-correlation (green; dy within +-S, dx within +-DX, coarse-to-fine).
   The join is made at the cut line, snapped to the darkest band within
   +-snap (the inter-frame gap). Positions are tracked in lines relative to
   window 0: e_k = e_{k-1} + s_k. */
#define STITCH_MAXSEG 64
typedef struct {
  v350_tiff_stream *out;
  int width, channels, bits, dpi;
  size_t row_bytes;
  int m, A, S, DX, snapR;    /* lines/pixels: margin, half match band, dy search, dx search, snap */
  int nseg, seg, exp_h, last;
  int hold_from;
  uint8_t *tail; int tail_n; /* last 2m lines of previous window */
  uint8_t *head; int head_n; /* first 2m lines of current window */
  int head_need;
  int seam_done;
  double e_cur;              /* position error of current window, lines (window 0 = 0) */
  int shift[STITCH_MAXSEG], dx[STITCH_MAXSEG], snap[STITCH_MAXSEG], flat[STITCH_MAXSEG];
  double ncc[STITCH_MAXSEG], err[STITCH_MAXSEG];
  int verbose;
} v350_stitch;

int  stitch_init(v350_stitch *st, v350_tiff_stream *out, int width, int channels, int bits,
                 int dpi, int nseg, int margin_lines, int verbose);
int  stitch_begin_segment(v350_stitch *st, int seg, int exp_h);   /* -1 if window too short */
int  stitch_line(void *ctx, const uint8_t *line, size_t bpl, int index);  /* v350_line_cb */
int  stitch_end_segment(v350_stitch *st, int got_h);
void stitch_free(v350_stitch *st);
#endif
