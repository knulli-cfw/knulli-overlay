#include "ov_inflate.h"

#include <string.h>

/* Canonical Huffman decoding with a 9-bit first-level table: the table covers
 * every code short enough to matter (literals, and the common length/distance
 * codes), and anything longer falls back to walking the code bit by bit. */
#define FAST_BITS 9
#define FAST_SIZE (1 << FAST_BITS)
#define MAX_SYMS  288

typedef struct {
    unsigned short counts[16];      /* number of codes of each length */
    unsigned short symbols[MAX_SYMS];   /* symbols ordered by (length, value) */
    short fast[FAST_SIZE];          /* (length << 9) | symbol, or -1 */
} huff;

typedef struct {
    const unsigned char *in;
    size_t in_len, in_pos;
    unsigned long bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t out_len, out_pos;
    int pad;                        /* zero bytes invented past the input */
    int err;                        /* sticky: bad data */
} inflator;

static void need_bits(inflator *s, int n)
{
    while (s->bitcnt < n) {
        unsigned long byte = 0;

        if (s->in_pos < s->in_len) {
            byte = s->in[s->in_pos++];
        } else if (++s->pad > 4) {
            /* Reading a little past the end is normal -- the decoder looks
             * ahead by up to two bytes -- but a lot of it means the stream was
             * cut short. */
            s->err = 1;
        }
        s->bitbuf |= byte << s->bitcnt;
        s->bitcnt += 8;
    }
}

static unsigned get_bits(inflator *s, int n)
{
    unsigned v;

    if (n == 0)
        return 0;
    need_bits(s, n);
    v = (unsigned)(s->bitbuf & ((1UL << n) - 1));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static unsigned reverse_bits(unsigned code, int len)
{
    unsigned r = 0;
    int i;

    for (i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Returns 0 for a complete code, >0 for an incomplete one (which is legal for
 * a distance tree with a single symbol) and -1 for an over-subscribed one. */
static int huff_build(huff *h, const unsigned char *lengths, int n)
{
    unsigned short offs[16];
    int len, i, left, code;
    unsigned si;

    memset(h->counts, 0, sizeof(h->counts));
    for (i = 0; i < n; i++)
        h->counts[lengths[i]]++;
    h->counts[0] = 0;

    left = 1;
    for (len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0)
            return -1;
    }

    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = (unsigned short)(offs[len] + h->counts[len]);
    for (i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offs[lengths[i]]++] = (unsigned short)i;

    for (i = 0; i < FAST_SIZE; i++)
        h->fast[i] = -1;
    code = 0;
    si = 0;
    for (len = 1; len < 16; len++) {
        int c;

        for (c = 0; c < h->counts[len]; c++, si++) {
            if (len <= FAST_BITS) {
                unsigned rev = reverse_bits((unsigned)code, len);
                unsigned j;

                for (j = rev; j < FAST_SIZE; j += 1u << len)
                    h->fast[j] = (short)((len << 9) | h->symbols[si]);
            }
            code++;
        }
        code <<= 1;
    }
    return left;
}

static int huff_decode(inflator *s, const huff *h)
{
    int code, first, index, len;
    short e;

    need_bits(s, FAST_BITS);
    e = h->fast[s->bitbuf & (FAST_SIZE - 1)];
    if (e >= 0) {
        len = e >> 9;
        s->bitbuf >>= len;
        s->bitcnt -= len;
        return e & 511;
    }

    /* Longer than the table: the classic length-by-length walk. */
    code = first = index = 0;
    for (len = 1; len < 16; len++) {
        code |= (int)get_bits(s, 1);
        if (code - first < h->counts[len])
            return h->symbols[index + (code - first)];
        index += h->counts[len];
        first = (first + h->counts[len]) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const unsigned short LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inflate_block(inflator *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);

        if (s->err || sym < 0)
            return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_len)
                return -1;
            s->out[s->out_pos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            unsigned len, d, from;
            int dsym;

            sym -= 257;
            if (sym >= 29)
                return -1;
            len = LEN_BASE[sym] + get_bits(s, LEN_EXTRA[sym]);
            dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30)
                return -1;
            d = DIST_BASE[dsym] + get_bits(s, DIST_EXTRA[dsym]);
            if (d > s->out_pos || s->out_pos + len > s->out_len)
                return -1;
            from = (unsigned)(s->out_pos - d);
            /* Overlapping copies are the point of LZ77: byte at a time. */
            while (len--)
                s->out[s->out_pos++] = s->out[from++];
        }
    }
}

static void fixed_trees(huff *lit, huff *dist)
{
    unsigned char lengths[MAX_SYMS];
    int i;

    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++)      lengths[i] = 9;
    for (; i < 280; i++)      lengths[i] = 7;
    for (; i < 288; i++)      lengths[i] = 8;
    huff_build(lit, lengths, 288);

    for (i = 0; i < 30; i++)
        lengths[i] = 5;
    huff_build(dist, lengths, 30);
}

static const unsigned char CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int dynamic_trees(inflator *s, huff *lit, huff *dist)
{
    unsigned char lengths[MAX_SYMS + 32];
    huff clen;
    unsigned nlen, ndist, ncode;
    unsigned i;

    nlen  = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return -1;

    memset(lengths, 0, 19);
    for (i = 0; i < ncode; i++)
        lengths[CLEN_ORDER[i]] = (unsigned char)get_bits(s, 3);
    if (huff_build(&clen, lengths, 19) != 0)
        return -1;

    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(s, &clen);
        unsigned rep;
        unsigned char v;

        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0)
                return -1;
            v = lengths[i - 1];
            rep = 3 + get_bits(s, 2);
        } else if (sym == 17) {
            v = 0;
            rep = 3 + get_bits(s, 3);
        } else {
            v = 0;
            rep = 11 + get_bits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = v;
    }
    if (lengths[256] == 0)          /* no end-of-block code */
        return -1;
    if (huff_build(lit, lengths, (int)nlen) != 0)
        return -1;
    /* An incomplete distance tree is fine as long as it is not used. */
    if (huff_build(dist, lengths + nlen, (int)ndist) < 0)
        return -1;
    return 0;
}

static long inflate_raw(inflator *s)
{
    int final;

    do {
        int type;

        final = (int)get_bits(s, 1);
        type = (int)get_bits(s, 2);
        if (s->err)
            return -1;
        if (type == 0) {                        /* stored */
            unsigned len;

            /* Back up over whatever the bit buffer had read ahead, then
             * discard the partial byte: a stored block starts byte aligned. */
            while (s->bitcnt >= 8 && s->in_pos > 0) {
                s->in_pos--;
                s->bitcnt -= 8;
            }
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->in_pos + 4 > s->in_len)
                return -1;
            len = s->in[s->in_pos] | ((unsigned)s->in[s->in_pos + 1] << 8);
            s->in_pos += 4;                     /* LEN then its complement */
            if (s->in_pos + len > s->in_len || s->out_pos + len > s->out_len)
                return -1;
            memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
            s->in_pos += len;
            s->out_pos += len;
        } else if (type == 1 || type == 2) {
            huff lit, dist;

            if (type == 1)
                fixed_trees(&lit, &dist);
            else if (dynamic_trees(s, &lit, &dist) != 0)
                return -1;
            if (inflate_block(s, &lit, &dist) != 0)
                return -1;
        } else {
            return -1;                          /* reserved */
        }
    } while (!final);

    return s->err ? -1 : (long)s->out_pos;
}

static unsigned adler32(const unsigned char *p, size_t n)
{
    unsigned a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

long ov_inflate_zlib(const unsigned char *in, size_t in_len,
                     unsigned char *out, size_t out_len)
{
    inflator s;
    long n;

    if (!in || !out || in_len < 2)
        return -1;
    /* RFC 1950: deflate method, window <= 32K, no preset dictionary. */
    if ((in[0] & 0x0f) != 8 || ((in[0] << 8 | in[1]) % 31) != 0 ||
        (in[1] & 0x20))
        return -1;

    memset(&s, 0, sizeof(s));
    s.in = in + 2;
    s.in_len = in_len - 2;
    s.out = out;
    s.out_len = out_len;

    n = inflate_raw(&s);
    if (n < 0)
        return -1;

    /* The trailer is the last four bytes of the stream, big endian; the bit
     * reader may have looked past it, so take it from the end rather than
     * from where the reader stopped. */
    if (s.in_len >= 4) {
        const unsigned char *t = s.in + s.in_len - 4;
        unsigned want = ((unsigned)t[0] << 24) | ((unsigned)t[1] << 16) |
                        ((unsigned)t[2] << 8) | t[3];

        if (want != adler32(out, (size_t)n))
            return -1;
    }
    return n;
}
