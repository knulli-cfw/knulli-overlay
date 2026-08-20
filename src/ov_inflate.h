/* DEFLATE / zlib decompression, just enough for PNG.
 *
 * The overlay library links libc and nothing else -- pulling in zlib would
 * mean the game process has to have a compatible one, which is exactly the
 * kind of assumption the rest of this code avoids.  The output size of a PNG
 * image stream is known before decoding, so the caller always supplies the
 * destination buffer and nothing here grows or reallocates.
 */
#ifndef OV_INFLATE_H
#define OV_INFLATE_H

#include <stddef.h>

/* Inflates a zlib stream (RFC 1950 header + RFC 1951 data) into `out`.
 * Returns the number of bytes written, or -1 on a malformed stream or if the
 * data would not fit.  The Adler-32 trailer is checked when present. */
long ov_inflate_zlib(const unsigned char *in, size_t in_len,
                     unsigned char *out, size_t out_len);

#endif /* OV_INFLATE_H */
