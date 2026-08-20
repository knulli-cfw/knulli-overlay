#include "ov_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_inflate.h"

/* Bezels for a 1080p screen are around 8MB decoded; the cap is there so a
 * corrupt header cannot ask for an unreasonable allocation. */
#define PNG_MAX_DIM   8192
#define PNG_MAX_FILE  (32u * 1024u * 1024u)

static const unsigned char PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long size;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 8 ||
        (unsigned long)size > PNG_MAX_FILE) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* Turns the filtered scanlines in `raw` into plain samples, in place. */
static int unfilter(unsigned char *raw, int h, size_t stride, int bpp)
{
    unsigned char *prev = NULL;
    unsigned char *line = raw;
    int y;
    size_t i;

    for (y = 0; y < h; y++) {
        int filter = line[0];
        unsigned char *cur = line + 1;

        switch (filter) {
        case 0:
            break;
        case 1:
            for (i = (size_t)bpp; i < stride; i++)
                cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
            break;
        case 2:
            if (prev)
                for (i = 0; i < stride; i++)
                    cur[i] = (unsigned char)(cur[i] + prev[i]);
            break;
        case 3:
            for (i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;

                cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;

                cur[i] = (unsigned char)(cur[i] + paeth(a, b, c));
            }
            break;
        default:
            return -1;
        }
        prev = cur;
        line += stride + 1;
    }
    return 0;
}

/* Expands one unfiltered scanline into RGBA8. */
static void expand_row(unsigned char *dst, const unsigned char *src, int w,
                       int colour, int depth, const unsigned char pal[256][4],
                       int has_key, const unsigned short key[3])
{
    int x;
    int step = depth == 16 ? 2 : 1;     /* 16 bit samples: keep the high byte */

    for (x = 0; x < w; x++) {
        unsigned char r = 0, g = 0, b = 0, a = 255;

        switch (colour) {
        case 0: {                       /* greyscale */
            const unsigned char *s = src + (size_t)x * step;

            r = g = b = s[0];
            if (has_key && key[0] == (depth == 16 ?
                    (unsigned short)((s[0] << 8) | s[1]) : s[0]))
                a = 0;
            break;
        }
        case 2: {                       /* truecolour */
            const unsigned char *s = src + (size_t)x * 3 * step;

            r = s[0];
            g = s[step];
            b = s[2 * step];
            if (has_key && key[0] == r && key[1] == g && key[2] == b)
                a = 0;
            break;
        }
        case 3: {                       /* palette */
            const unsigned char *p = pal[src[x]];

            r = p[0];
            g = p[1];
            b = p[2];
            a = p[3];
            break;
        }
        case 4: {                       /* greyscale + alpha */
            const unsigned char *s = src + (size_t)x * 2 * step;

            r = g = b = s[0];
            a = s[step];
            break;
        }
        default: {                      /* 6: truecolour + alpha */
            const unsigned char *s = src + (size_t)x * 4 * step;

            r = s[0];
            g = s[step];
            b = s[2 * step];
            a = s[3 * step];
            break;
        }
        }
        *dst++ = r;
        *dst++ = g;
        *dst++ = b;
        *dst++ = a;
    }
}

/* Palette entries with 1/2/4 bit indices, and sub-byte greyscale, are
 * unpacked to one byte per sample first so expand_row only ever sees 8 or 16
 * bit samples. */
static void unpack_bits(unsigned char *dst, const unsigned char *src, int n,
                        int depth, int scale_to_8)
{
    int i;
    int per_byte = 8 / depth;
    int mask = (1 << depth) - 1;
    int max = mask;

    for (i = 0; i < n; i++) {
        int byte = src[i / per_byte];
        int shift = 8 - depth * (i % per_byte + 1);
        int v = (byte >> shift) & mask;

        dst[i] = (unsigned char)(scale_to_8 ? v * 255 / max : v);
    }
}

int ov_png_load(const char *path, ov_image *out)
{
    unsigned char *file = NULL, *idat = NULL, *raw = NULL, *rgba = NULL;
    unsigned char *unpacked = NULL;
    unsigned char pal[256][4];
    unsigned short key[3] = { 0, 0, 0 };
    size_t file_len = 0, idat_len = 0, idat_cap = 0, stride;
    unsigned w = 0, h = 0;
    int depth = 0, colour = 0, interlace = 0, channels, bpp, has_key = 0;
    int y;
    size_t pos;
    long got;

    memset(out, 0, sizeof(*out));
    memset(pal, 0, sizeof(pal));

    file = read_file(path, &file_len);
    if (!file) {
        fprintf(stderr, "knulli-overlay: cannot read %s\n", path);
        return -1;
    }
    if (memcmp(file, PNG_SIG, 8) != 0) {
        fprintf(stderr, "knulli-overlay: %s is not a PNG\n", path);
        goto fail;
    }

    /* One pass to size the compressed data, so it can be joined in one go:
     * encoders split IDAT into chunks at arbitrary points. */
    for (pos = 8; pos + 8 <= file_len; ) {
        unsigned len = be32(file + pos);

        if (len > file_len || pos + 12 + len > file_len)
            break;
        if (memcmp(file + pos + 4, "IDAT", 4) == 0)
            idat_cap += len;
        pos += 12 + len;
    }
    if (idat_cap == 0) {
        fprintf(stderr, "knulli-overlay: %s has no image data\n", path);
        goto fail;
    }
    idat = malloc(idat_cap);
    if (!idat)
        goto fail;

    for (pos = 8; pos + 8 <= file_len; ) {
        unsigned len = be32(file + pos);
        const unsigned char *type = file + pos + 4;
        const unsigned char *data = file + pos + 8;

        if (len > file_len || pos + 12 + len > file_len)
            break;
        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            colour = data[9];
            interlace = data[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            unsigned i;

            for (i = 0; i < len / 3 && i < 256; i++) {
                pal[i][0] = data[i * 3];
                pal[i][1] = data[i * 3 + 1];
                pal[i][2] = data[i * 3 + 2];
                pal[i][3] = 255;
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            unsigned i;

            if (colour == 3) {
                for (i = 0; i < len && i < 256; i++)
                    pal[i][3] = data[i];
            } else if (colour == 0 && len >= 2) {
                key[0] = (unsigned short)((data[0] << 8) | data[1]);
                if (depth != 16)
                    key[0] = (unsigned short)(key[0] & 0xff);
                has_key = 1;
            } else if (colour == 2 && len >= 6) {
                for (i = 0; i < 3; i++)
                    key[i] = (unsigned short)(data[i * 2 + 1]);
                has_key = 1;
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }

    if (w == 0 || h == 0 || w > PNG_MAX_DIM || h > PNG_MAX_DIM) {
        fprintf(stderr, "knulli-overlay: %s has an unusable size %ux%u\n",
                path, w, h);
        goto fail;
    }
    if (interlace) {
        fprintf(stderr, "knulli-overlay: %s is interlaced, not supported\n",
                path);
        goto fail;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
        fprintf(stderr, "knulli-overlay: %s has bit depth %d\n", path, depth);
        goto fail;
    }
    switch (colour) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default:
        fprintf(stderr, "knulli-overlay: %s has colour type %d\n", path, colour);
        goto fail;
    }
    if (colour != 0 && colour != 3 && depth < 8) {
        fprintf(stderr, "knulli-overlay: %s: %d bit colour type %d\n",
                path, depth, colour);
        goto fail;
    }

    stride = ((size_t)w * channels * depth + 7) / 8;
    bpp = (channels * depth + 7) / 8;       /* filter step, at least 1 byte */

    if (depth < 8) {
        unpacked = malloc(w);
        if (!unpacked)
            goto fail;
        /* A grey transparency key is given in the file's own scale. */
        if (colour == 0 && has_key)
            key[0] = (unsigned short)(key[0] * 255 / ((1 << depth) - 1));
    }
    raw = malloc((stride + 1) * (size_t)h);
    rgba = malloc((size_t)w * h * 4);
    if (!raw || !rgba)
        goto fail;

    got = ov_inflate_zlib(idat, idat_len, raw, (stride + 1) * (size_t)h);
    if (got != (long)((stride + 1) * (size_t)h)) {
        fprintf(stderr, "knulli-overlay: %s: bad compressed data\n", path);
        goto fail;
    }
    if (unfilter(raw, (int)h, stride, bpp) != 0) {
        fprintf(stderr, "knulli-overlay: %s: bad scanline filter\n", path);
        goto fail;
    }

    for (y = 0; (unsigned)y < h; y++) {
        const unsigned char *line = raw + (stride + 1) * (size_t)y + 1;

        if (depth < 8) {
            /* Palette indices stay indices; grey levels scale to 0..255. */
            unpack_bits(unpacked, line, (int)w, depth, colour != 3);
            expand_row(rgba + (size_t)y * w * 4, unpacked, (int)w, colour, 8,
                       pal, has_key, key);
        } else {
            expand_row(rgba + (size_t)y * w * 4, line, (int)w, colour, depth,
                       pal, has_key, key);
        }
    }

    free(unpacked);
    free(raw);
    free(idat);
    free(file);
    out->w = (int)w;
    out->h = (int)h;
    out->rgba = rgba;
    return 0;

fail:
    free(unpacked);
    free(rgba);
    free(raw);
    free(idat);
    free(file);
    memset(out, 0, sizeof(*out));
    return -1;
}

void ov_image_free(ov_image *img)
{
    if (!img)
        return;
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}
