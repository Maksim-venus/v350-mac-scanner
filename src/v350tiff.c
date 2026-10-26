/* v350tiff.c — PPM/TIFF writers (no CoreFoundation/libusb deps) */
#include "v350lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int v350_write_ppm(const v350_image_t *im, const char *path) {
  if (im->bits != 8) return -1;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  if (im->channels == 3)
    fprintf(f, "P6\n%d %d\n255\n", im->width, im->height);
  else
    fprintf(f, "P5\n%d %d\n255\n", im->width, im->height);
  fwrite(im->data, 1, im->nbytes, f);
  fclose(f);
  return 0;
}

/* Minimal uncompressed TIFF (8 or 16-bit, gray or RGB, LE). */
int v350_write_tiff(const v350_image_t *im, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int spp = im->channels;
  int bps = im->bits;
  uint32_t strip = (uint32_t)im->nbytes;
  /* IFD with 11 entries */
  uint16_t ntags = 11;
  uint32_t ifd_off = 8;
  uint8_t header[8] = { 'I','I', 42,0, 8,0,0,0 };
  fwrite(header, 1, 8, f);

  typedef struct { uint16_t tag, type; uint32_t count, value; } T;
  T tags[12];
  int i = 0;
  #define TAG(tg,ty,c,v) do{ tags[i].tag=tg; tags[i].type=ty; tags[i].count=c; tags[i].value=v; i++; }while(0)
  TAG(256, 3, 1, (uint32_t)im->width);           /* ImageWidth SHORT */
  TAG(257, 3, 1, (uint32_t)im->height);          /* ImageLength */
  TAG(258, 3, (uint32_t)spp, 0);                 /* BitsPerSample — offset if spp>1 */
  TAG(259, 3, 1, 1);                             /* Compression none */
  TAG(262, 3, 1, spp == 3 ? 2 : 1);              /* Photometric RGB=2 Gray=1 */
  TAG(273, 4, 1, 0);                             /* StripOffsets — fill later */
  TAG(277, 3, 1, (uint32_t)spp);                 /* SamplesPerPixel */
  TAG(278, 4, 1, (uint32_t)im->height);          /* RowsPerStrip */
  TAG(279, 4, 1, strip);                         /* StripByteCounts */
  TAG(282, 5, 1, 0);                             /* XResolution rational offset */
  TAG(283, 5, 1, 0);                             /* YResolution */
  TAG(296, 3, 1, 2);                             /* ResolutionUnit = inch */
  #undef TAG
  ntags = (uint16_t)i;

  uint32_t after_ifd = ifd_off + 2 + ntags * 12 + 4;
  /* BitsPerSample array if spp==3 */
  uint32_t bps_off = after_ifd;
  uint32_t xres_off, yres_off, data_off;
  if (spp == 3) {
    tags[2].value = bps_off;
    after_ifd += 6; /* 3 * SHORT */
  } else {
    tags[2].value = (uint32_t)bps; /* inline SHORT */
  }
  xres_off = after_ifd; after_ifd += 8;
  yres_off = after_ifd; after_ifd += 8;
  data_off = after_ifd;
  tags[5].value = data_off; /* StripOffsets */
  tags[9].value = xres_off;
  tags[10].value = yres_off;

  fwrite(&ntags, 2, 1, f);
  for (int t = 0; t < ntags; t++) {
    fwrite(&tags[t].tag, 2, 1, f);
    fwrite(&tags[t].type, 2, 1, f);
    fwrite(&tags[t].count, 4, 1, f);
    fwrite(&tags[t].value, 4, 1, f);
  }
  uint32_t next = 0;
  fwrite(&next, 4, 1, f);

  if (spp == 3) {
    uint16_t b[3] = { (uint16_t)bps, (uint16_t)bps, (uint16_t)bps };
    fwrite(b, 2, 3, f);
  }
  uint32_t rat[2] = { (uint32_t)im->dpi_x, 1 };
  fwrite(rat, 4, 2, f);
  rat[0] = (uint32_t)im->dpi_y;
  fwrite(rat, 4, 2, f);
  fwrite(im->data, 1, im->nbytes, f);
  fclose(f);
  return 0;
}

/* ---------------- streaming writer ---------------- */
struct v350_tiff_stream {
  FILE *f;
  int width, bits, channels, rows;
  size_t row_bytes;
  long off_len, off_rps, off_sbc;
};

static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void tag(FILE *f, uint16_t tg, uint16_t ty, uint32_t cnt, uint32_t val) {
  put16(f, tg); put16(f, ty); put32(f, cnt);
  if (ty == 3 && cnt == 1) { put16(f, (uint16_t)val); put16(f, 0); } else put32(f, val);
}

v350_tiff_stream *v350_tiff_open(const char *path, int width, int bits, int channels, int dpi) {
  v350_tiff_stream *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->f = fopen(path, "wb");
  if (!t->f) { free(t); return NULL; }
  t->width = width; t->bits = bits; t->channels = channels;
  t->row_bytes = (size_t)width * channels * (bits / 8);
  FILE *f = t->f;
  const uint16_t ntags = 13;
  uint32_t ifd = 8, after = ifd + 2 + ntags * 12 + 4;
  uint32_t bps_off = after, xres_off = bps_off + 8, yres_off = xres_off + 8, data_off = yres_off + 8;
  fwrite("II", 1, 2, f); put16(f, 42); put32(f, ifd);
  put16(f, ntags);
  tag(f, 256, 4, 1, (uint32_t)width);                          /* ImageWidth LONG */
  t->off_len = ftell(f) + 8; tag(f, 257, 4, 1, 0);              /* ImageLength (patched) */
  if (channels == 3) tag(f, 258, 3, 3, bps_off); else tag(f, 258, 3, 1, (uint32_t)bits);
  tag(f, 259, 3, 1, 1);                                         /* no compression */
  tag(f, 262, 3, 1, channels == 3 ? 2 : 1);
  tag(f, 273, 4, 1, data_off);                                  /* StripOffsets */
  tag(f, 277, 3, 1, (uint32_t)channels);
  t->off_rps = ftell(f) + 8; tag(f, 278, 4, 1, 0);              /* RowsPerStrip (patched) */
  t->off_sbc = ftell(f) + 8; tag(f, 279, 4, 1, 0);              /* StripByteCounts (patched) */
  tag(f, 282, 5, 1, xres_off);
  tag(f, 283, 5, 1, yres_off);
  tag(f, 284, 3, 1, 1);                                         /* PlanarConfig contig */
  tag(f, 296, 3, 1, 2);                                         /* ResolutionUnit inch */
  put32(f, 0);                                                  /* next IFD */
  put16(f, (uint16_t)bits); put16(f, (uint16_t)bits); put16(f, (uint16_t)bits); put16(f, 0);
  put32(f, (uint32_t)dpi); put32(f, 1);
  put32(f, (uint32_t)dpi); put32(f, 1);
  if (ftell(f) != (long)data_off) { fclose(f); free(t); return NULL; }
  return t;
}
int v350_tiff_write_rows(v350_tiff_stream *t, const uint8_t *rows, int nrows) {
  if (nrows <= 0) return 0;
  if (fwrite(rows, t->row_bytes, (size_t)nrows, t->f) != (size_t)nrows) return -1;
  t->rows += nrows;
  return 0;
}
int v350_tiff_close(v350_tiff_stream *t, int *out_height) {
  uint64_t bytes = (uint64_t)t->row_bytes * t->rows;
  int rc = 0;
  if (bytes > 0xFFFFFFFFull) rc = -1;
  fseek(t->f, t->off_len, SEEK_SET); put32(t->f, (uint32_t)t->rows);
  fseek(t->f, t->off_rps, SEEK_SET); put32(t->f, (uint32_t)t->rows);
  fseek(t->f, t->off_sbc, SEEK_SET); put32(t->f, (uint32_t)bytes);
  if (fclose(t->f) != 0) rc = -1;
  if (out_height) *out_height = t->rows;
  free(t);
  return rc;
}
