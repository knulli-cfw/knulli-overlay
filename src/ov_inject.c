/* LD_PRELOAD shim: draws the overlay into every frame the host process
 * presents.
 *
 * Hooking eglSwapBuffers is deliberate: on these handhelds RetroArch, ES and
 * the standalone emulators all end up there whatever they sit on (fbdev, DRM
 * /GBM, Wayland, X11) and whether they drive EGL directly or through SDL2.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ov_bezel.h"
#include "ov_config.h"
#include "ov_gl.h"
#include "ov_layout.h"
#include "ov_state.h"

/* The library is built with -fvisibility=hidden; only the hooks get out. */
#ifndef OV_EXPORT
#define OV_EXPORT __attribute__((visibility("default")))
#endif

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH  0x3057

#define EGL_DRAW 0x3059

typedef EGLBoolean (*swap_fn)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*swap_damage_fn)(EGLDisplay, EGLSurface, EGLint *, EGLint);
typedef void (*sdl_swap_fn)(void *);

/* One renderer per GL context; a handful of slots covers the emulators that
 * juggle a second context for their menu or for shared resources. */
#define OV_MAX_CTX 4

static struct {
    EGLContext ctx;
    ov_gl *gl;
} g_ctx[OV_MAX_CTX];

static ov_reader g_reader;
static ov_bezel g_bezel;
static ov_style g_style;
/* What the colours and size are when nothing has been published yet: the clock
 * and the bezel are up before any key has been pressed, so they cannot wait for
 * an event to carry the settings across. */
static ov_snapshot g_idle;
static int g_clock_show, g_clock_always, g_clock_12h;
static char g_clock_text[16];
static time_t g_clock_minute = -1;
static int g_rotation;
static int g_enabled = -1;      /* -1 = not yet initialised */
static __thread int g_in_hook;

static EGLContext (*real_eglGetCurrentContext)(void);
static EGLDisplay (*real_eglGetCurrentDisplay)(void);
static EGLSurface (*real_eglGetCurrentSurface)(EGLint);
static EGLBoolean (*real_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint,
                                          EGLint *);
static void *(*real_eglGetProcAddress)(const char *);

/* EGL may only be reachable through a dlopen()ed handle (SDL2), so fall back
 * to the renderer's resolver rather than the link chain alone. */
static void *egl_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);

    return p ? p : ov_gl_default_getproc(name);
}

/* Formats the wall clock, at most once a minute.  Returns NULL when the clock
 * is switched off. */
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

static void ov_init(void)
{
    const char *rot = getenv("OV_ROTATE");
    ov_config cfg;

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

    real_eglGetProcAddress = dlsym(RTLD_NEXT, "eglGetProcAddress");
    ov_gl_set_egl_getproc(real_eglGetProcAddress);
    real_eglGetCurrentContext = egl_sym("eglGetCurrentContext");
    real_eglGetCurrentDisplay = egl_sym("eglGetCurrentDisplay");
    real_eglGetCurrentSurface = egl_sym("eglGetCurrentSurface");
    real_eglQuerySurface = egl_sym("eglQuerySurface");
    if (!real_eglGetCurrentContext || !real_eglQuerySurface) {
        fprintf(stderr, "knulli-overlay: no EGL in this process, disabled\n");
        g_enabled = 0;
    }
}

static ov_gl *renderer_for(EGLContext ctx)
{
    int i, victim = 0;

    for (i = 0; i < OV_MAX_CTX; i++) {
        if (g_ctx[i].ctx == ctx)
            return g_ctx[i].gl;
        if (!g_ctx[i].ctx)
            victim = i;
    }
    if (g_ctx[victim].gl) {
        /* Slot reuse: the old context is gone, so its objects went with it. */
        ov_gl_destroy(g_ctx[victim].gl);
        g_ctx[victim].gl = NULL;
    }
    g_ctx[victim].ctx = ctx;
    g_ctx[victim].gl = ov_gl_create(NULL);
    return g_ctx[victim].gl;
}

static void draw_overlay(EGLDisplay dpy, EGLSurface surf)
{
    ov_snapshot snap;
    ov_drawlist dl;
    EGLContext ctx;
    ov_gl *gl;
    EGLint w = 0, h = 0;
    int have_state, have_bezel;

    if (g_enabled < 0)
        ov_init();
    if (!g_enabled || g_in_hook)
        return;
    have_state = ov_reader_poll(&g_reader, &snap);
    if (!have_state)
        snap = g_idle;              /* colours and size, with nothing shown */
    /* Neither the bezel nor the clock is an event: once a game has a bezel it
     * is part of every frame, and the clock is up whether or not anything has
     * been published, so a frame with nothing else to show still gets drawn. */
    have_bezel = ov_bezel_poll(&g_bezel);
    g_style.clock_text = (have_state || g_clock_always) ? clock_text() : NULL;
    if (!have_state && !have_bezel && !g_style.clock_text)
        return;

    ctx = real_eglGetCurrentContext();
    if (!ctx)
        return;
    if (!real_eglQuerySurface(dpy, surf, EGL_WIDTH, &w) ||
        !real_eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) || w <= 0 || h <= 0)
        return;

    /* The panel is laid out in screen space, so a rotated display swaps the
     * dimensions before layout and the shader turns the result back. */
    if (g_rotation & 1) {
        EGLint t = w;
        w = h;
        h = t;
    }

    g_in_hook = 1;
    gl = renderer_for(ctx);
    if (gl) {
        ov_layout_reset(&dl);
        /* Each context has its own texture, so the upload is keyed on the
         * bezel's generation rather than done once globally. */
        if (ov_gl_image_gen(gl) != g_bezel.gen)
            ov_gl_set_image(gl, have_bezel ? g_bezel.img.rgba : NULL,
                            g_bezel.img.w, g_bezel.img.h, g_bezel.gen);
        if (have_bezel) {
            float bx, by, bw, bh;

            ov_bezel_rect(&g_bezel, w, h, &bx, &by, &bw, &bh);
            ov_layout_add_image(&dl, bx, by, bw, bh, g_bezel.opacity);
        }
        /* The build draws whatever the snapshot and the style ask for, which
         * with nothing published is just the clock. */
        ov_layout_build(&dl, &snap, &g_style, w, h);
        ov_gl_draw(gl, &dl, w, h, g_rotation);
    }
    g_in_hook = 0;
}

/* For hooks that are not given a display/surface, e.g. SDL_GL_SwapWindow. */
static void draw_overlay_current(void)
{
    if (g_enabled < 0)
        ov_init();
    if (!g_enabled || !real_eglGetCurrentDisplay || !real_eglGetCurrentSurface)
        return;
    draw_overlay(real_eglGetCurrentDisplay(),
                 real_eglGetCurrentSurface(EGL_DRAW));
}

OV_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
{
    static swap_fn real;

    if (!real)
        real = (swap_fn)dlsym(RTLD_NEXT, "eglSwapBuffers");
    draw_overlay(dpy, surf);
    return real ? real(dpy, surf) : 0;
}

OV_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf,
                                       EGLint *rects, EGLint n_rects)
{
    static swap_damage_fn real;

    (void)rects;
    (void)n_rects;

    if (!real)
        real = (swap_damage_fn)dlsym(RTLD_NEXT, "eglSwapBuffersWithDamageKHR");
    draw_overlay(dpy, surf);
    /* Our rectangle is outside the damage the app declared, so let the whole
     * frame go out rather than posting a partial update. */
    return real ? real(dpy, surf, NULL, 0) : 0;
}

OV_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surf,
                                       EGLint *rects, EGLint n_rects)
{
    static swap_damage_fn real;

    (void)rects;
    (void)n_rects;

    if (!real)
        real = (swap_damage_fn)dlsym(RTLD_NEXT, "eglSwapBuffersWithDamageEXT");
    draw_overlay(dpy, surf);
    return real ? real(dpy, surf, NULL, 0) : 0;
}

/* SDL2 reaches EGL through its own dlopen()ed handle, so this is where the
 * RetroArch/EmulationStation frames are caught. */
OV_EXPORT void SDL_GL_SwapWindow(void *window)
{
    static sdl_swap_fn real;

    if (!real)
        real = (sdl_swap_fn)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    draw_overlay_current();
    if (real)
        real(window);
}

/* Apps that fetch swap through eglGetProcAddress must get our version too. */
OV_EXPORT void *eglGetProcAddress(const char *name)
{
    if (!real_eglGetProcAddress)
        real_eglGetProcAddress = dlsym(RTLD_NEXT, "eglGetProcAddress");
    ov_gl_set_egl_getproc(real_eglGetProcAddress);
    if (name) {
        if (!strcmp(name, "eglSwapBuffers"))
            return (void *)eglSwapBuffers;
        if (!strcmp(name, "eglSwapBuffersWithDamageKHR"))
            return (void *)eglSwapBuffersWithDamageKHR;
        if (!strcmp(name, "eglSwapBuffersWithDamageEXT"))
            return (void *)eglSwapBuffersWithDamageEXT;
    }
    return real_eglGetProcAddress ? real_eglGetProcAddress(name) : NULL;
}
