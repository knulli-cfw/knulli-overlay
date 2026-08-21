#include "ov_frame.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ov_config.h"

static ov_reader g_reader;
static ov_bezel  g_bezel;
static ov_style  g_style;
/* What the colours and size are when nothing has been published yet: the clock
 * and the bezel are up before any key has been pressed, so they cannot wait
 * for an event to carry the settings across. */
static ov_snapshot g_idle;
static int g_clock_show, g_clock_always, g_clock_12h;
static char g_clock_text[16];
static time_t g_clock_minute = -1;
static int g_rotation;
static int g_debug;
static int g_enabled = -1;      /* -1 = not yet initialised */

void ov_log(const char *fmt, ...)
{
    va_list ap;

    if (!g_debug)
        return;
    fprintf(stderr, "knulli-overlay: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Formats the wall clock, at most once a minute.  NULL when the clock is
 * switched off. */
static const char *clock_text(void)
{
    time_t now;
    struct tm tm;
    int hour;

    if (!g_clock_show)
        return NULL;
    now = time(NULL);
    if (now / 60 == g_clock_minute)
        return g_clock_text;        /* same minute, same string */
    g_clock_minute = now / 60;
    if (!localtime_r(&now, &tm))
        return NULL;

    hour = tm.tm_hour;
    if (!g_clock_12h) {
        snprintf(g_clock_text, sizeof(g_clock_text), "%02d:%02d", hour,
                 tm.tm_min);
    } else {
        const char *suffix = hour < 12 ? "AM" : "PM";

        hour %= 12;
        if (!hour)
            hour = 12;
        snprintf(g_clock_text, sizeof(g_clock_text), "%d:%02d %s", hour,
                 tm.tm_min, suffix);
    }
    return g_clock_text;
}

void ov_frame_init(void)
{
    const char *rot = getenv("OV_ROTATE");
    ov_config cfg;

    if (g_enabled >= 0)
        return;

    g_debug = getenv("OV_DEBUG") && atoi(getenv("OV_DEBUG")) != 0;
    g_enabled = !(getenv("OV_DISABLE") && atoi(getenv("OV_DISABLE")) != 0);
    ov_reader_init(&g_reader);
    ov_bezel_init(&g_bezel);
    ov_style_defaults(&g_style);

    /* The clock is ours to draw rather than the CLI's to publish, so its
     * settings -- and the colours everything falls back to -- are read here. */
    ov_config_load(&cfg);
    g_clock_show = cfg.enabled && cfg.clock_show;
    g_clock_always = cfg.clock_always;
    g_clock_12h = cfg.clock_12h;
    if (g_style.clock_anchor < 0)
        g_style.clock_anchor = cfg.clock_anchor;
    ov_snapshot_defaults(&g_idle);
    g_idle.fg = cfg.fg;
    g_idle.bg = cfg.bg;
    g_idle.alpha = (uint32_t)(cfg.alpha * 255.0f + 0.5f);
    g_idle.scale = (uint32_t)(cfg.scale * 1000.0f + 0.5f);
    g_rotation = rot ? (atoi(rot) / 90) & 3 : 0;
}

int ov_frame_poll(ov_frame *f)
{
    int have_state;

    ov_frame_init();
    if (!g_enabled)
        return 0;
    have_state = ov_reader_poll(&g_reader, &f->snap);
    if (!have_state)
        f->snap = g_idle;           /* colours and size, with nothing shown */
    /* Neither the bezel nor the clock is an event: once a game has a bezel it
     * is part of every frame, and the clock is up whether or not anything has
     * been published, so a frame with nothing else to show still gets drawn. */
    f->have_bezel = ov_bezel_poll(&g_bezel);
    g_style.clock_text = (have_state || g_clock_always) ? clock_text() : NULL;
    return have_state || f->have_bezel || g_style.clock_text != NULL;
}

void ov_frame_build(ov_drawlist *dl, const ov_frame *f, int w, int h)
{
    ov_layout_reset(dl);
    if (f->have_bezel) {
        float bx, by, bw, bh;

        ov_bezel_rect(&g_bezel, w, h, &bx, &by, &bw, &bh);
        ov_layout_add_image(dl, bx, by, bw, bh, g_bezel.opacity,
                            g_bezel.hole);
    }
    /* The build draws whatever the snapshot and the style ask for, which with
     * nothing published is just the clock. */
    ov_layout_build(dl, &f->snap, &g_style, w, h);
}

const ov_bezel *ov_frame_bezel(void)
{
    return &g_bezel;
}

int ov_frame_rotation(void)
{
    return g_rotation;
}

int ov_frame_enabled(void)
{
    return g_enabled > 0;
}
