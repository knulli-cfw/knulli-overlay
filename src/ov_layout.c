#include "ov_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_anchor.h"
#include "ov_atlas.h"

/* The overlay is sized from the geometric mean of the screen's two dimensions
 * rather than its height alone.  Height alone makes the widgets eat a quarter
 * of the width on a square 720x720 panel while looking right on a 1280x720 one;
 * the geometric mean tracks both, and matches the old size exactly at 16:9.
 * The caps below then stop anything dominating an extreme aspect ratio. */
#define P_PANEL_H     0.255f    /* panel height, of sqrt(w * h) */
#define P_PANEL_MAX_H 0.45f     /* ... but never this much of the height */
#define P_PANEL_MAX_W 0.22f     /* ... nor this much of the width */
/* The pill holds three icons and a number now, so this is headroom for the
 * emergency case rather than a size it normally reaches: shrinking it would
 * also make its text smaller than the clock's beside it. */
#define B_MAX_W       0.34f     /* status pill width, of the screen width */

/* Notification banner.  Its text is the shared size like everything else; only
 * the padding and the shrink-to-fit are its own. */
#define N_PAD_X       0.75f     /* padding, of the cap height */
#define N_PAD_Y       0.55f
#define N_MAX_W       0.86f     /* of the screen width, before shrinking */
#define N_MIN_SHRINK  0.6f      /* how small the text may get before eliding */

/* The caps are there to keep the *automatic* size sane on an odd aspect ratio,
 * so they move with overlay.scale: a screen whose pixels are physically larger
 * or smaller can still be dialled up or down without hitting a ceiling meant
 * for something else. */

/* Proportions taken from the mockup (a 71x145 panel). */
#define P_ASPECT      (71.0f / 145.0f)
#define P_RADIUS      0.085f    /* of panel width */
#define P_BORDER      0.014f    /* of panel width */
#define P_ICON_W      0.338f    /* of panel width */
#define P_ICON_H      0.131f    /* of panel height */
#define P_ICON_TOP    0.028f
#define P_BAR_X       0.099f    /* of panel width */
#define P_BAR_W       0.803f
#define P_BAR_TOP     0.172f    /* of panel height */
#define P_BAR_H       0.090f
#define P_BAR_STEP    0.128f
#define P_TEXT_CAP    0.131f    /* digit cap height, of panel height */
#define P_TEXT_BOTTOM 0.945f
#define P_SPACE       0.35f     /* word space, as a fraction of a digit advance */
#define P_TEXT_MAX_W  0.78f     /* of panel width, before the text is shrunk */

/* Battery pill, top left.  Sizes are relative to its text cap height, which is
 * the same as the volume panel's so the two read as one family. */
#define B_PAD         0.55f     /* inner padding */
#define B_GAP         0.38f     /* space between icon, shell and text */
#define B_SHELL_W     1.85f     /* battery body, relative to its height */
#define B_NUB_W       0.13f
#define B_NUB_H       0.42f
#define B_RADIUS      0.22f     /* of the shell height */
#define B_RADIO       0.86f     /* wifi/bluetooth icon height, of the cap */
#define B_BOLT        0.80f     /* charging bolt, of the shell height */
#define B_BOLT_HALO   1.22f     /* the card-coloured outline behind it */

/* '0' sits this far below the ascender line; used to align every glyph. */
#define ATLAS_REF_TOP (ov_atlas_glyphs[OV_G_D0].top)

/* How far the hairline edge sits from the ink, towards the card.  With the
 * default black-on-white that lands on the mockup's grey; with any other pair
 * it stays a muted version of the same relationship. */
#define P_EDGE_MIX 0.62f

/* Ink is the bars, icons and text; card is what they sit on. */
typedef struct {
    float ink[4];
    float card[4];
    float edge[4];
    float card_alpha;
} ov_palette;

static void unpack_rgb(float *out, unsigned rgb)
{
    out[0] = (float)((rgb >> 16) & 0xff) / 255.0f;
    out[1] = (float)((rgb >> 8) & 0xff) / 255.0f;
    out[2] = (float)(rgb & 0xff) / 255.0f;
    out[3] = 1.0f;
}

static void palette_make(ov_palette *p, unsigned fg, unsigned bg, float alpha)
{
    int i;

    unpack_rgb(p->ink, fg);
    unpack_rgb(p->card, bg);
    for (i = 0; i < 3; i++)
        p->edge[i] = p->ink[i] + (p->card[i] - p->ink[i]) * P_EDGE_MIX;
    p->edge[3] = 1.0f;
    p->card_alpha = alpha;
}

/* Every piece of text in the overlay is drawn at one size, so the pill, the
 * clock and the panel read as one family and their boxes line up.  The size is
 * derived from the widest thing that has to fit -- "100 %" inside the panel --
 * rather than from what is on screen right now, so it does not change as the
 * value climbs or as icons come and go. */
static float shared_text_cap(float ph, float pw);

/* Places a w x h box against one of the nine anchors, inset from the edges. */
static void place(float *x, float *y, float w, float h, int anchor,
                  float inset, float screen_w, float screen_h)
{
    switch (OV_ANCHOR_H(anchor)) {
    case OV_H_CENTER: *x = (screen_w - w) * 0.5f;   break;
    case OV_H_RIGHT:  *x = screen_w - w - inset;    break;
    default:          *x = inset;                   break;
    }
    switch (OV_ANCHOR_V(anchor)) {
    case OV_V_MIDDLE: *y = (screen_h - h) * 0.5f;   break;
    case OV_V_BOTTOM: *y = screen_h - h - inset;    break;
    default:          *y = inset;                   break;
    }
}

static int env_anchor(const char *name)
{
    const char *v = getenv(name);

    return (v && *v) ? ov_anchor_parse(v, -1) : -1;
}

static int pick(int override, uint32_t published)
{
    return override >= 0 ? override : (int)published;
}

static unsigned pick_colour(unsigned override, uint32_t published)
{
    return override != OV_COLOUR_UNSET ? override : published;
}

static unsigned env_colour(const char *name)
{
    const char *v = getenv(name);
    unsigned rgb;

    if (!v || !*v)
        return OV_COLOUR_UNSET;
    if (*v == '#')
        v++;
    if (sscanf(v, "%6x", &rgb) != 1 || strlen(v) != 6)
        return OV_COLOUR_UNSET;
    return rgb;
}

static float env_float(const char *name, float fallback, float lo, float hi)
{
    const char *v = getenv(name);
    float f;

    if (!v || !*v)
        return fallback;
    f = strtof(v, NULL);
    if (f < lo)
        f = lo;
    if (f > hi)
        f = hi;
    return f;
}

void ov_style_defaults(ov_style *st)
{
    st->scale      = getenv("OV_SCALE")
                     ? env_float("OV_SCALE", OV_DEFAULT_SCALE, 0.25f, 4.0f)
                     : -1.0f;
    st->margin_pct = env_float("OV_MARGIN_PCT", 0.04f, 0.0f, 0.5f);
    st->segments   = (int)env_float("OV_SEGMENTS", 5.0f, 1.0f, 20.0f);
    st->bg_alpha   = getenv("OV_BG_ALPHA")
                     ? env_float("OV_BG_ALPHA", OV_DEFAULT_ALPHA, 0.0f, 1.0f)
                     : -1.0f;
    st->fg = env_colour("OV_FGCOLOR");
    st->bg = env_colour("OV_BGCOLOR");

    /* Testing overrides: what knulli.conf says wins unless one is set. */
    st->clock_text = NULL;
    st->clock_anchor = env_anchor("OV_CLOCK_POS");
    st->panel_anchor = env_anchor("OV_PANEL_POS");
    st->battery_anchor = env_anchor("OV_BATTERY_POS");
    st->notification_anchor = env_anchor("OV_NOTIFICATION_POS");
    /* OV_INVERT is shorthand for the two colours swapping, same as the config
     * key; an explicit OV_FGCOLOR/OV_BGCOLOR still wins. */
    st->invert = getenv("OV_INVERT") ? (atoi(getenv("OV_INVERT")) != 0) : -1;
    if (st->invert >= 0) {
        if (st->fg == OV_COLOUR_UNSET)
            st->fg = st->invert ? OV_COLOUR_WHITE : OV_COLOUR_BLACK;
        if (st->bg == OV_COLOUR_UNSET)
            st->bg = st->invert ? OV_COLOUR_BLACK : OV_COLOUR_WHITE;
    }
}

static ov_cmd *push(ov_drawlist *dl)
{
    ov_cmd *c;

    if (dl->count >= OV_MAX_CMDS)
        return NULL;
    c = &dl->cmd[dl->count++];
    memset(c, 0, sizeof(*c));
    c->glyph = -1;
    c->fill[3] = 1.0f;
    return c;
}

static void set_rgba(float *dst, const float *src, float alpha)
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = alpha;
}

/* Lays out `text` at `cap_h`, left aligned.  Returns the width; a NULL
 * drawlist measures without drawing.  `limit` characters at most. */
static float draw_text(ov_drawlist *dl, const char *text, size_t limit,
                       float x, float cap_bottom, float cap_h,
                       const float *colour)
{
    float scale = cap_h / (float)ov_atlas_glyphs[OV_G_D0].h;
    float ascender = cap_bottom - cap_h - ATLAS_REF_TOP * scale;
    float pen = x;
    size_t i;

    for (i = 0; i < limit && text[i]; i++) {
        int gi = OV_GLYPH_FOR((unsigned char)text[i]);
        const ov_glyph *g = &ov_atlas_glyphs[gi];
        ov_cmd *c;

        if (dl && g->w > 1 && g->h > 1) {       /* skip the blank cells */
            c = push(dl);
            if (!c)
                break;
            c->glyph = gi;
            c->x = pen;
            c->y = ascender + g->top * scale;
            c->w = g->w * scale;
            c->h = g->h * scale;
            set_rgba(c->fill, colour, 1.0f);
        }
        pen += g->advance * scale;
    }
    return pen - x;
}

/* How many characters of `text` fit in `width` at `cap_h`. */
static size_t text_fits(const char *text, float width, float cap_h)
{
    float scale = cap_h / (float)ov_atlas_glyphs[OV_G_D0].h;
    float pen = 0.0f;
    size_t i;

    for (i = 0; text[i]; i++) {
        pen += ov_atlas_glyphs[OV_GLYPH_FOR((unsigned char)text[i])].advance * scale;
        if (pen > width)
            return i;
    }
    return i;
}

/* Lays out the digits of `value` followed by " %".  Returns the width; pass a
 * NULL drawlist to measure without drawing. */
static float percent_text(ov_drawlist *dl, int value, float x, float cap_bottom,
                          float cap_h, const float *colour)
{
    int glyphs[8];
    int n = 0, i;
    float scale, width = 0.0f, pen, space, ascender;
    char digits[8];
    int len = 0;

    if (value < 0)
        value = 0;
    do {
        digits[len++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && len < (int)sizeof(digits));
    while (len > 0)
        glyphs[n++] = OV_GLYPH_FOR((unsigned char)digits[--len]);
    glyphs[n++] = -1;               /* space */
    glyphs[n++] = OV_G_PERCENT;

    scale = cap_h / (float)ov_atlas_glyphs[OV_G_D0].h;
    space = ov_atlas_glyphs[OV_G_SPACE].advance;
    for (i = 0; i < n; i++)
        width += (glyphs[i] < 0 ? space : ov_atlas_glyphs[glyphs[i]].advance) * scale;
    if (!dl)
        return width;

    pen = x;
    ascender = cap_bottom - cap_h - ATLAS_REF_TOP * scale;
    for (i = 0; i < n; i++) {
        const ov_glyph *g;
        ov_cmd *c;

        if (glyphs[i] < 0) {
            pen += space * scale;
            continue;
        }
        g = &ov_atlas_glyphs[glyphs[i]];
        c = push(dl);
        if (!c)
            break;
        c->glyph = glyphs[i];
        c->x = pen;
        c->y = ascender + g->top * scale;
        c->w = g->w * scale;
        c->h = g->h * scale;
        set_rgba(c->fill, colour, 1.0f);
        pen += g->advance * scale;
    }
    return width;
}

/* The battery indicator: a pill in the top left holding an optional charging
 * bolt, a battery whose fill follows the charge, and the percentage.  No
 * segment bars -- it is meant to be glanceable, not the main event. */
static void add_battery(ov_drawlist *dl, const ov_snapshot *snap,
                        const ov_palette *pal, int anchor,
                        float cap_h, float inset, float border,
                        float screen_w, float screen_h, float scale)
{
    int charging = (snap->battery_flags & OV_FLAG_CHARGING) != 0;
    int wifi = (snap->battery_flags & OV_FLAG_WIFI) != 0;
    int bt = (snap->battery_flags & OV_FLAG_BLUETOOTH) != 0;
    int gauge = snap->battery != OV_BATTERY_NONE;
    int percent = gauge && !(snap->battery_flags & OV_FLAG_NO_PERCENT);
    float pad = cap_h * B_PAD;
    float gap = cap_h * B_GAP;
    float shell_h = cap_h;
    float shell_w = gauge ? shell_h * B_SHELL_W : 0.0f;
    float nub_w = gauge ? shell_h * B_NUB_W : 0.0f;
    float bolt_w = 0.0f;
    float wifi_w = 0.0f, bt_w = 0.0f;
    float text_w = percent ? percent_text(NULL, snap->battery, 0, 0, cap_h, NULL)
                           : 0.0f;
    float w, h, x, y, cx, inner, fill_w;
    ov_cmd *c;

    if (charging)
        bolt_w = cap_h * (float)ov_atlas_glyphs[OV_G_ICON_BOLT].w /
                 (float)ov_atlas_glyphs[OV_G_ICON_BOLT].h;
    /* The radio icons are cut a little smaller than the digits: at the same
     * cap height they read as heavier than the battery beside them. */
    if (wifi)
        wifi_w = cap_h * B_RADIO * (float)ov_atlas_glyphs[OV_G_ICON_WIFI].w /
                 (float)ov_atlas_glyphs[OV_G_ICON_WIFI].h;
    if (bt)
        bt_w = cap_h * B_RADIO * (float)ov_atlas_glyphs[OV_G_ICON_BLUETOOTH].w /
               (float)ov_atlas_glyphs[OV_G_ICON_BLUETOOTH].h;

    /* Charging is drawn on the battery itself, so it costs no width -- unless
     * there is no battery to draw it on. */
    w = pad * 2.0f + shell_w + nub_w + text_w + (percent ? gap : 0.0f) +
        (charging && !gauge ? bolt_w + gap : 0.0f) +
        (wifi ? wifi_w + gap : 0.0f) + (bt ? bt_w + gap : 0.0f);
    if (!gauge)
        w -= gap;                   /* no trailing gap after the last icon */

    /* A square or narrow screen would otherwise hand the pill a quarter of its
     * width; shrink the whole thing rather than clip or wrap it. */
    if (w > screen_w * B_MAX_W * scale) {
        float shrink = screen_w * B_MAX_W * scale / w;

        cap_h *= shrink;
        pad *= shrink;
        gap *= shrink;
        shell_h *= shrink;
        shell_w *= shrink;
        nub_w *= shrink;
        bolt_w *= shrink;
        wifi_w *= shrink;
        bt_w *= shrink;
        w = screen_w * B_MAX_W * scale;
    }
    h = pad * 2.0f + cap_h;
    place(&x, &y, w, h, anchor, inset, screen_w, screen_h);

    /* card */
    c = push(dl);
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->radius = h * 0.5f;
    c->border = border;
    set_rgba(c->fill, pal->card, pal->card_alpha);
    set_rgba(c->stroke, pal->edge, 1.0f);

    cx = x + pad;

    /* Status-bar order: the radios first, then the battery and its level. */
    if (wifi) {
        c = push(dl);
        c->glyph = OV_G_ICON_WIFI;
        c->x = cx;
        c->y = y + pad + (cap_h - cap_h * B_RADIO) * 0.5f;
        c->w = wifi_w;
        c->h = cap_h * B_RADIO;
        set_rgba(c->fill, pal->ink, 1.0f);
        cx += wifi_w + gap;
    }
    if (bt) {
        c = push(dl);
        c->glyph = OV_G_ICON_BLUETOOTH;
        c->x = cx;
        c->y = y + pad + (cap_h - cap_h * B_RADIO) * 0.5f;
        c->w = bt_w;
        c->h = cap_h * B_RADIO;
        set_rgba(c->fill, pal->ink, 1.0f);
        cx += bt_w + gap;
    }

    /* With three icons in the pill a fourth one beside them reads as another
     * status icon rather than as the battery's state, so the bolt goes on the
     * battery -- except when there is no battery to put it on. */
    if (charging && !gauge) {
        c = push(dl);
        c->glyph = OV_G_ICON_BOLT;
        c->x = cx;
        c->y = y + pad;
        c->w = bolt_w;
        c->h = cap_h;
        set_rgba(c->fill, pal->ink, 1.0f);
        cx += bolt_w + gap;
    }

    if (!gauge)
        return;

    /* shell */
    c = push(dl);
    c->x = cx;
    c->y = y + pad;
    c->w = shell_w;
    c->h = shell_h;
    c->radius = shell_h * B_RADIUS;
    c->border = border;
    set_rgba(c->fill, pal->card, 0.0f);
    set_rgba(c->stroke, pal->ink, 1.0f);

    /* nub */
    c = push(dl);
    c->x = cx + shell_w;
    c->y = y + pad + shell_h * (1.0f - B_NUB_H) * 0.5f;
    c->w = nub_w;
    c->h = shell_h * B_NUB_H;
    c->radius = nub_w * 0.4f;
    set_rgba(c->fill, pal->ink, 1.0f);

    /* charge level */
    inner = shell_w - border * 4.0f;
    fill_w = inner * (float)(snap->battery < 0 ? 0 : snap->battery) / 100.0f;
    if (fill_w > 0.0f) {
        c = push(dl);
        c->x = cx + border * 2.0f;
        c->y = y + pad + border * 2.0f;
        c->w = fill_w;
        c->h = shell_h - border * 4.0f;
        c->radius = c->h * 0.25f;
        set_rgba(c->fill, pal->ink, 1.0f);
    }

    /* The bolt is drawn twice: a slightly larger one in the card colour, then
     * the ink one inside it.  Over the filled part of the shell the card halo
     * is what shows, as an outline around an invisible ink core; over the empty
     * part the halo disappears and the ink bolt shows solid.  Either way there
     * is a bolt, whatever the charge and whichever colours are configured. */
    if (charging) {
        float bolt_h = shell_h * B_BOLT;
        float bx, by;
        int i;

        bolt_w = bolt_h * (float)ov_atlas_glyphs[OV_G_ICON_BOLT].w /
                 (float)ov_atlas_glyphs[OV_G_ICON_BOLT].h;
        bx = cx + (shell_w - bolt_w) * 0.5f;
        by = y + pad + (shell_h - bolt_h) * 0.5f;

        for (i = 0; i < 2; i++) {
            float grow = i == 0 ? B_BOLT_HALO : 1.0f;

            c = push(dl);
            c->glyph = OV_G_ICON_BOLT;
            c->w = bolt_w * grow;
            c->h = bolt_h * grow;
            c->x = bx - (c->w - bolt_w) * 0.5f;
            c->y = by - (c->h - bolt_h) * 0.5f;
            set_rgba(c->fill, i == 0 ? pal->card : pal->ink, 1.0f);
        }
    }

    if (percent) {
        cx += shell_w + nub_w + gap;
        percent_text(dl, snap->battery, cx, y + pad + cap_h, cap_h, pal->ink);
    }
}

/* The clock: the same pill as the battery, holding the time instead.  It is
 * the one element with nothing to publish -- see ov_style.clock_text. */
static void add_clock(ov_drawlist *dl, const char *text,
                      const ov_palette *pal, int anchor, float cap_h,
                      float inset, float border,
                      float screen_w, float screen_h)
{
    float pad = cap_h * B_PAD;
    float text_w = draw_text(NULL, text, strlen(text), 0, 0, cap_h, NULL);
    float w = text_w + pad * 2.0f;
    float h = cap_h + pad * 2.0f;
    float x, y;
    ov_cmd *c;

    place(&x, &y, w, h, anchor, inset, screen_w, screen_h);

    c = push(dl);
    if (!c)
        return;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->radius = h * 0.5f;
    c->border = border;
    set_rgba(c->fill, pal->card, pal->card_alpha);
    set_rgba(c->stroke, pal->edge, 1.0f);

    draw_text(dl, text, strlen(text), x + pad, y + pad + cap_h, cap_h,
              pal->ink);
}

/* A single-line banner holding the notification text.  Long text shrinks to
 * N_MIN_SHRINK and is then cut with an ellipsis rather than wrapping: these are
 * one-liners, and a growing box would fight with the panels for the screen. */
static void add_notification(ov_drawlist *dl, const char *text,
                             const ov_palette *pal, int anchor, float cap_h,
                             float inset, float border,
                             float screen_w, float screen_h)
{
    static const char ellipsis[] = "...";
    float max_text = screen_w * N_MAX_W;
    float pad_x, pad_y, text_w, w, h, x, y;
    size_t len = strlen(text), shown;
    ov_cmd *c;

    text_w = draw_text(NULL, text, len, 0, 0, cap_h, NULL);
    max_text -= cap_h * N_PAD_X * 2.0f;

    if (text_w > max_text) {
        float shrink = max_text / text_w;

        if (shrink < N_MIN_SHRINK)
            shrink = N_MIN_SHRINK;
        cap_h *= shrink;
        text_w = draw_text(NULL, text, len, 0, 0, cap_h, NULL);
    }

    shown = len;
    if (text_w > max_text) {        /* still too long: elide */
        float dots = draw_text(NULL, ellipsis, 3, 0, 0, cap_h, NULL);

        shown = text_fits(text, max_text - dots, cap_h);
        text_w = draw_text(NULL, text, shown, 0, 0, cap_h, NULL) + dots;
    }

    pad_x = cap_h * N_PAD_X;
    pad_y = cap_h * N_PAD_Y;
    w = text_w + pad_x * 2.0f;
    h = cap_h + pad_y * 2.0f;
    place(&x, &y, w, h, anchor, inset, screen_w, screen_h);

    c = push(dl);
    if (!c)
        return;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->radius = h * 0.5f;           /* a pill, like the battery indicator */
    c->border = border;
    set_rgba(c->fill, pal->card, pal->card_alpha);
    set_rgba(c->stroke, pal->edge, 1.0f);

    x += pad_x + draw_text(dl, text, shown, x + pad_x, y + pad_y + cap_h,
                           cap_h, pal->ink);
    if (shown < len)
        draw_text(dl, ellipsis, 3, x, y + pad_y + cap_h, cap_h, pal->ink);
}

void ov_layout_reset(ov_drawlist *dl)
{
    dl->count = 0;
}

/* One piece of the image: a plain textured quad, no rounding, no border, tinted
 * white so only its own colours and alpha reach the fragment. */
static void add_image_part(ov_drawlist *dl, float x, float y, float w, float h,
                           float u, float v, float du, float dv, float alpha)
{
    ov_cmd *c;

    if (dl->count >= OV_MAX_CMDS || w <= 0.5f || h <= 0.5f)
        return;
    c = &dl->cmd[dl->count++];
    memset(c, 0, sizeof(*c));
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->glyph = -1;
    c->image = 1;
    c->uv[0] = u;
    c->uv[1] = v;
    c->uv[2] = du;
    c->uv[3] = dv;
    c->fill[0] = c->fill[1] = c->fill[2] = 1.0f;
    c->fill[3] = alpha > 1.0f ? 1.0f : alpha;
}

void ov_layout_add_image(ov_drawlist *dl, float x, float y, float w, float h,
                         float alpha, const float *hole)
{
    float hx0, hy0, hx1, hy1;

    if (w <= 0.0f || h <= 0.0f || alpha <= 0.0f)
        return;
    if (!hole || hole[2] <= hole[0] || hole[3] <= hole[1]) {
        add_image_part(dl, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, alpha);
        return;
    }

    /* The hole in screen space; what is left is up to four strips around it,
     * each with the matching piece of the image. */
    hx0 = x + hole[0] * w;
    hy0 = y + hole[1] * h;
    hx1 = x + hole[2] * w;
    hy1 = y + hole[3] * h;

    add_image_part(dl, x, y, w, hy0 - y,
                   0.0f, 0.0f, 1.0f, hole[1], alpha);                 /* top */
    add_image_part(dl, x, hy1, w, y + h - hy1,
                   0.0f, hole[3], 1.0f, 1.0f - hole[3], alpha);    /* bottom */
    add_image_part(dl, x, hy0, hx0 - x, hy1 - hy0,
                   0.0f, hole[1], hole[0], hole[3] - hole[1], alpha);/* left */
    add_image_part(dl, hx1, hy0, x + w - hx1, hy1 - hy0,
                   hole[2], hole[1], 1.0f - hole[2], hole[3] - hole[1],
                   alpha);                                          /* right */
}

void ov_layout_build(ov_drawlist *dl, const ov_snapshot *snap,
                     const ov_style *st, int screen_w, int screen_h)
{
    float ph, pw, px, py, radius, border, inset, scale, cap;
    float bar_x, bar_w, bar_h, step;
    int filled, i, icon;
    ov_palette pal;
    ov_cmd *c;

    if (!snap || screen_w <= 0 || screen_h <= 0)
        return;

    palette_make(&pal,
                 pick_colour(st->fg, snap->fg),
                 pick_colour(st->bg, snap->bg),
                 st->bg_alpha >= 0.0f ? st->bg_alpha
                                      : (float)snap->alpha / 255.0f);
    scale = st->scale >= 0.0f ? st->scale : (float)snap->scale / 1000.0f;
    if (scale <= 0.0f)
        scale = OV_DEFAULT_SCALE;
    ph = sqrtf((float)screen_w * (float)screen_h) * P_PANEL_H * scale;
    if (ph > (float)screen_h * P_PANEL_MAX_H * scale)
        ph = (float)screen_h * P_PANEL_MAX_H * scale;
    pw = ph * P_ASPECT;
    if (pw > (float)screen_w * P_PANEL_MAX_W * scale) {
        pw = (float)screen_w * P_PANEL_MAX_W * scale;
        ph = pw / P_ASPECT;
    }
    inset = (float)screen_w * st->margin_pct;
    place(&px, &py, pw, ph, pick(st->panel_anchor, snap->panel_anchor),
          inset, (float)screen_w, (float)screen_h);
    radius = pw * P_RADIUS;
    border = pw * P_BORDER;
    if (border < 1.0f)
        border = 1.0f;
    cap = shared_text_cap(ph, pw);

    /* The status pill -- battery, and the wifi/bluetooth icons that share it --
     * rides along with a volume or brightness event as well as appearing on its
     * own, so it is laid out independently of the panel. */
    if (snap->battery != OV_BATTERY_NONE ||
        (snap->battery_flags & OV_FLAG_RADIOS))
        add_battery(dl, snap, &pal,
                    pick(st->battery_anchor, snap->battery_anchor),
                    cap, inset, border,
                    (float)screen_w, (float)screen_h, scale);

    if (st->clock_text && st->clock_text[0])
        add_clock(dl, st->clock_text, &pal,
                  pick(st->clock_anchor, OV_ANCHOR_DEFAULT_CLOCK),
                  cap, inset, border,
                  (float)screen_w, (float)screen_h);

    if (snap->text[0])
        add_notification(dl, snap->text, &pal,
                         pick(st->notification_anchor, snap->notification_anchor),
                         cap, inset, border,
                         (float)screen_w, (float)screen_h);

    if (snap->kind == OV_KIND_NONE)
        return;

    /* panel */
    c = push(dl);
    c->x = px;
    c->y = py;
    c->w = pw;
    c->h = ph;
    c->radius = radius;
    c->border = border;
    set_rgba(c->fill, pal.card, pal.card_alpha);
    set_rgba(c->stroke, pal.edge, 1.0f);

    /* icon */
    icon = (snap->kind == OV_KIND_BRIGHTNESS) ? OV_G_ICON_SUN : OV_G_ICON_SPEAKER;
    c = push(dl);
    c->glyph = icon;
    c->h = ph * P_ICON_H;
    c->w = c->h * (float)ov_atlas_glyphs[icon].w / (float)ov_atlas_glyphs[icon].h;
    if (c->w > pw * P_ICON_W) {     /* keep the icon inside its box */
        c->w = pw * P_ICON_W;
        c->h = c->w * (float)ov_atlas_glyphs[icon].h / (float)ov_atlas_glyphs[icon].w;
    }
    c->x = px + (pw - c->w) * 0.5f;
    c->y = py + ph * P_ICON_TOP + (ph * P_ICON_H - c->h) * 0.5f;
    set_rgba(c->fill, pal.ink, 1.0f);

    /* segment bars, filling from the bottom */
    bar_x = px + pw * P_BAR_X;
    bar_w = pw * P_BAR_W;
    bar_h = ph * P_BAR_H;
    step = ph * P_BAR_STEP;
    filled = 0;
    if (snap->value > 0 && !(snap->flags & OV_FLAG_MUTED)) {
        filled = (int)((snap->value * st->segments + 50) / 100);
        if (filled < 1)
            filled = 1;
        if (filled > st->segments)
            filled = st->segments;
    }
    for (i = 0; i < st->segments; i++) {
        int from_bottom = st->segments - 1 - i;   /* row 0 is the top one */

        c = push(dl);
        if (!c)
            break;
        c->x = bar_x;
        c->y = py + ph * P_BAR_TOP + step * i;
        c->w = bar_w;
        c->h = bar_h;
        c->radius = 0.0f;
        c->border = border;
        if (from_bottom < filled) {
            set_rgba(c->fill, pal.ink, 1.0f);
            set_rgba(c->stroke, pal.ink, 1.0f);
        } else {
            set_rgba(c->fill, pal.card, 0.0f);
            set_rgba(c->stroke, pal.edge, 1.0f);
        }
    }

    {
        float tw = percent_text(NULL, snap->value, 0, 0, cap, NULL);

        percent_text(dl, snap->value, px + (pw - tw) * 0.5f,
                     py + ph * P_TEXT_BOTTOM, cap, pal.ink);
    }
}

/* Defined here because it measures with percent_text(). */
static float shared_text_cap(float ph, float pw)
{
    float cap = ph * P_TEXT_CAP;
    float room = pw * P_TEXT_MAX_W;
    /* 100 is the widest the panel ever shows; sizing for it means the digits
     * keep one size all the way up rather than dropping a notch at 100. */
    float widest = percent_text(NULL, 100, 0, 0, cap, NULL);

    if (widest > room)
        cap *= room / widest;
    return cap;
}
