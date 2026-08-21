/* Turns an overlay state into a small list of drawing commands.
 *
 * The layout is resolution independent: every measurement is a fraction of the
 * panel box, whose size is derived from the screen.  Rendering back-ends only
 * have to know how to draw a rounded rectangle and a glyph quad.
 */
#ifndef OV_LAYOUT_H
#define OV_LAYOUT_H

#include "ov_state.h"

/* bezel (1) + panel (11) + status pill (12) + clock (6) + notification text */
#define OV_MAX_CMDS 170

typedef struct {
    float x, y, w, h;     /* pixels, origin top-left of the screen */
    float radius;         /* corner radius, pixels */
    float border;         /* border thickness, pixels (0 = filled only) */
    float fill[4];        /* RGBA, straight alpha */
    float stroke[4];      /* RGBA of the border */
    int   glyph;          /* atlas glyph index, or -1 for a rounded rect */
    int   image;          /* 1: draw the bezel texture, `fill` tinting it */
    float uv[4];          /* image sub-rectangle: u, v, du, dv (image only) */
} ov_cmd;

typedef struct {
    ov_cmd cmd[OV_MAX_CMDS];
    int count;
} ov_drawlist;

/* Appearance knobs, all overridable through the environment so a device can
 * tune the overlay without a rebuild. */
typedef struct {
    float scale;          /* size multiplier on the automatic size, -1 unset */
    float margin_pct;     /* screen-edge inset as a fraction of the width */
    int   segments;       /* number of bar segments */
    /* Appearance comes from knulli.conf through the shared state.  These are
     * $OV_* overrides for testing and are negative / OV_COLOUR_UNSET when the
     * environment says nothing. */
    int      panel_anchor;
    int      battery_anchor;
    int      notification_anchor;
    /* The clock is not published by anyone: the library formats the current
     * time each frame and points this at it (NULL draws no clock). */
    const char *clock_text;
    int      clock_anchor;
    int      invert;
    unsigned fg;
    unsigned bg;
    float    bg_alpha;
} ov_style;

void ov_style_defaults(ov_style *st);   /* built-in values, then $OV_* overrides */

/* The drawing commands are appended, so a caller can put the bezel underneath
 * the widgets by adding it first. */
void ov_layout_reset(ov_drawlist *dl);

/* Adds an image.  `hole`, when not NULL, is a rectangle of the image that is
 * fully transparent, as u0, v0, u1, v1 in 0..1: the image is then drawn as the
 * frame around it rather than as one quad over the whole screen.  A bezel is
 * mostly hole, and on a tile-based GPU the pixels not drawn are the ones that
 * cost. */
void ov_layout_add_image(ov_drawlist *dl, float x, float y, float w, float h,
                         float alpha, const float *hole);

void ov_layout_build(ov_drawlist *dl, const ov_snapshot *snap,
                     const ov_style *st, int screen_w, int screen_h);

#endif /* OV_LAYOUT_H */
