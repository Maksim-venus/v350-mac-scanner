#ifndef V350LIB_H
#define V350LIB_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define V350_VID 0x04b8
#define V350_PID 0x012f

/* Scan area limits (1/100 inch) accepted for each source. */
#define V350_AFL_W100 145   /* 1.45" */
#define V350_AFL_H100 930   /* 9.30" AFL maximum */
#define V350_TPU_W100 145
#define V350_TPU_H100 481   /* 4.81" == ESC f reported option area */

typedef enum { V350_SRC_FLATBED = 0, V350_SRC_TPU = 1, V350_SRC_AFL = 5 } v350_source_t;
typedef enum { V350_FILM_POS = 0, V350_FILM_NEG = 1 } v350_film_t;
typedef enum { V350_COLOR_RGB = 0x13, V350_COLOR_GRAY = 0x10 } v350_color_t;

typedef struct {
  int verbose;
  int require_high_speed;   /* refuse open before INTInit unless USB 2.0 High-Speed */
  char err[256];
} v350_t;

typedef struct {
  int width, height, dpi_x, dpi_y, bits, channels;
  uint8_t *data;            /* row-major; 16-bit samples little-endian */
  size_t nbytes;
} v350_image_t;

typedef struct {
  int fatal;                /* ESC F / ESC f fatal bit */
  int warming;              /* ESC f byte0 bit1 */
  int afl_installed;        /* ESC f byte6 0x80 (option unit present) */
  int afl_enabled;          /* ESC f byte6 0x40 */
  unsigned area_x, area_y;  /* ESC f option area, 4800-dpi units */
} v350_status_t;

/* Scan outcome, so callers know what is still safe to send. */
typedef enum {
  V350_OK = 0,
  V350_ERR_CLEAN = 1,       /* failed/cancelled but protocol is in a clean state */
  V350_ERR_WEDGED = 2       /* USB stall/timeout mid-transfer: send NOTHING more, power-cycle */
} v350_rc_t;

/* Epson interpreter location (from the official ICA driver). Override: v350_set_interpreter()
   or env V350_INTERPRETER (path to Interpreter 68.bundle, EPSON Scanner.app or its folder). */
void v350_set_interpreter(const char *path);
const char *v350_find_interpreter(void);       /* NULL if not installed */
int  v350_firmware_present(const char *bundle);
int  v350_open(v350_t *s);          /* USB open + speed check + INTInit + ESC @ */
void v350_close(v350_t *s);         /* INTClose + libusb release; sends no ESC commands */
int  v350_usb_speed(void);          /* libusb speed enum of open device */
int  v350_is_wedged(void);

int  v350_status(v350_t *s, v350_status_t *st);
int  v350_wait_ready(v350_t *s, int timeout_s);
int  v350_set_source(v350_t *s, v350_source_t src);
int  v350_release(v350_t *s);       /* ESC e 0 */
int  v350_eject(v350_t *s);         /* experimental: ESC w, then ESC e 0 (did not eject on the test unit) */

void v350_request_abort(void);      /* async-signal-safe; CAN at next block */

/* Per-line callback: return non-zero to abort (CAN at block boundary). */
typedef int (*v350_line_cb)(void *ctx, const uint8_t *line, size_t bpl, int index);

/* ESC @ + ESC e 5 + ready poll (repeat before every window). */
int v350_prepare_afl(v350_t *s);

/* One AFL window: full 1.45" width, y/h in pixels at dpi (ESC A units). */
v350_rc_t v350_scan_window(v350_t *s, int dpi, int bits, v350_color_t color, v350_film_t film,
                           int y_px, int h_px, v350_line_cb cb, void *ctx, int *out_w, int *out_h);

void v350_image_free(v350_image_t *im);
int  v350_write_ppm(const v350_image_t *im, const char *path);
int  v350_write_tiff(const v350_image_t *im, const char *path);

/* Streaming TIFF (uncompressed, single strip, height patched on close). */
typedef struct v350_tiff_stream v350_tiff_stream;
v350_tiff_stream *v350_tiff_open(const char *path, int width, int bits, int channels, int dpi);
int  v350_tiff_write_rows(v350_tiff_stream *t, const uint8_t *rows, int nrows);
int  v350_tiff_close(v350_tiff_stream *t, int *out_height);  /* patches ImageLength etc. */
#endif
