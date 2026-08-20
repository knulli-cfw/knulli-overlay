/* A small PNG reader, for bezel images.
 *
 * Decodes to straight (non-premultiplied) RGBA8, which is what the renderer
 * uploads.  Everything a bezel actually uses is supported -- 8 and 16 bit
 * greyscale, RGB, palette and alpha variants -- but not interlaced files;
 * those are rejected rather than half-drawn.
 */
#ifndef OV_PNG_H
#define OV_PNG_H

typedef struct {
    int w, h;
    unsigned char *rgba;    /* w * h * 4, top row first */
} ov_image;

/* 0 on success.  On failure `out` is zeroed and a line is written to stderr. */
int  ov_png_load(const char *path, ov_image *out);
void ov_image_free(ov_image *img);

#endif /* OV_PNG_H */
