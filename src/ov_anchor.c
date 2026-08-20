#include "ov_anchor.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

struct word {
    const char *name;
    int axis;       /* 0 = horizontal, 1 = vertical */
    int value;
};

static const struct word WORDS[] = {
    { "left",   0, OV_H_LEFT },
    { "center", 0, OV_H_CENTER },
    { "centre", 0, OV_H_CENTER },
    { "right",  0, OV_H_RIGHT },
    { "top",    1, OV_V_TOP },
    { "middle", 1, OV_V_MIDDLE },
    { "bottom", 1, OV_V_BOTTOM },
    { NULL, 0, 0 }
};

int ov_anchor_parse(const char *s, int fallback)
{
    int h = OV_H_CENTER, v = OV_V_MIDDLE;
    int got_h = 0, got_v = 0;
    char tok[16];
    size_t n = 0;
    int i, done = 0;

    if (!s)
        return fallback;

    while (!done) {
        int c = (unsigned char)*s++;

        if (c == '\0')
            done = 1;
        if (isalpha(c)) {
            if (n < sizeof(tok) - 1)
                tok[n++] = (char)tolower(c);
            continue;
        }
        if (!n)
            continue;

        tok[n] = '\0';
        n = 0;
        for (i = 0; WORDS[i].name; i++) {
            if (strcmp(tok, WORDS[i].name))
                continue;
            if (WORDS[i].axis == 0) {
                h = WORDS[i].value;
                got_h = 1;
            } else {
                v = WORDS[i].value;
                got_v = 1;
            }
            break;
        }
        if (!WORDS[i].name)
            return fallback;        /* an unknown word: do not guess */
    }

    if (!got_h && !got_v)
        return fallback;
    return OV_ANCHOR(h, v);
}

const char *ov_anchor_name(int anchor)
{
    static const char *const H[] = { "left", "center", "right" };
    static const char *const V[] = { "top", "middle", "bottom" };
    static char buf[24];

    snprintf(buf, sizeof(buf), "%s-%s", V[OV_ANCHOR_V(anchor)],
             H[OV_ANCHOR_H(anchor)]);
    return buf;
}
