#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* Decoded PNG image structure */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels; /* ARGB pixel data */
} PngImage;

/* Decode a PNG file from raw bytes into an image structure.
 * Returns 0 on success, negative on error.
 * Call png_free() when done with the image.
*/

int png_decode(const uint8_t *data, uint32_t data_len, PngImage *img);

/* Error codes returned by png_decode (0 = success). */
#define PNG_OK            0
#define PNG_ERR_BAD_SIG  -1
#define PNG_ERR_TRUNC    -2
#define PNG_ERR_BAD_IHDR -3
#define PNG_ERR_UNSUPP   -4
#define PNG_ERR_NOMEM    -5
#define PNG_ERR_INFLATE  -6
#define PNG_ERR_FILTER   -7
#define PNG_ERR_OVERFLOW -8
#define PNG_ERR_ADLER    -9

/* Human-readable name for a png_decode error code. */
const char *png_error_string(int code);

/* Free decoded PNG image memory */
void png_free(PngImage *img);

/* Draw a pre-decoded icon at screen coordinates, scaled to dest_w x dest_h.
 * Uses nearest-neighbor scaling. Skips transparent pixels. */
void gpu_draw_icon(const uint32_t *pixels, int img_w, int img_h,
                   int dest_x, int dest_y, int dest_w, int dest_h);

/* Draw a pre-decoded icon within a window's client area */
void wm_draw_icon(void *win, const uint32_t *pixels, int img_w, int img_h,
                  int x, int y, int w, int h);

#endif
