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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ov_frame.h"
#include "ov_gl.h"

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
/* SDL types are opaque here on purpose: the library builds without SDL
 * headers, the same way it builds without GL ones. */
typedef void (*sdl_present_fn)(void *renderer);
typedef int (*sdl_output_size_fn)(void *renderer, int *w, int *h);
typedef int (*sdl_flush_fn)(void *renderer);

/* One renderer per GL context; a handful of slots covers the emulators that
 * juggle a second context for their menu or for shared resources. */
#define OV_MAX_CTX 4

static struct {
    EGLContext ctx;
    ov_gl *gl;
} g_ctx[OV_MAX_CTX];

static int g_seen_egl, g_seen_sdl_gl, g_seen_sdl_ren, g_seen_sdl_sw;
static int g_enabled = -1;      /* -1 = not yet initialised */
static int g_rotation;
static __thread int g_in_hook;

static EGLContext (*real_eglGetCurrentContext)(void);
static EGLDisplay (*real_eglGetCurrentDisplay)(void);
static EGLSurface (*real_eglGetCurrentSurface)(EGLint);
static EGLBoolean (*real_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint,
                                          EGLint *);
static void *(*real_eglGetProcAddress)(const char *);

/* Everything SDL, resolved the same way EGL is: the process has SDL2 loaded
 * already, we only need the pointers. */
static void *sdl_sym(const char *name)
{
    static const char *const libs[] = {
        "libSDL2-2.0.so.0", "libSDL2.so", NULL
    };
    void *p = dlsym(RTLD_NEXT, name);
    int i;

    if (p)
        return p;
    p = dlsym(RTLD_DEFAULT, name);
    for (i = 0; libs[i] && !p; i++) {
        void *h = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);

        /* RTLD_NOLOAD: if the process has no SDL2 there is nothing to hook,
         * and loading a second copy would be worse than doing nothing. */
        if (h)
            p = dlsym(h, name);
    }
    return p;
}

/* EGL may only be reachable through a dlopen()ed handle (SDL2), so fall back
 * to the renderer's resolver rather than the link chain alone. */
static void *egl_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);

    return p ? p : ov_gl_default_getproc(name);
}

static void ov_init(void)
{
    ov_frame_init();
    g_enabled = ov_frame_enabled();
    g_rotation = ov_frame_rotation();

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
    ov_log("init: rotation %d", g_rotation * 90);
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

/* The front half of every hook: is there anything on this frame? */
static int frame_wanted(ov_frame *f)
{
    if (g_enabled < 0)
        ov_init();
    if (!g_enabled || g_in_hook)
        return 0;
    return ov_frame_poll(f);
}

/* The back half: draw into whichever GL context is current, at a size the
 * caller has already worked out. */
static void draw_gl(EGLContext ctx, int w, int h, const ov_frame *f)
{
    const ov_bezel *bezel = ov_frame_bezel();
    ov_drawlist dl;
    ov_gl *gl;

    /* The panel is laid out in screen space, so a rotated display swaps the
     * dimensions before layout and the shader turns the result back. */
    if (g_rotation & 1) {
        int t = w;
        w = h;
        h = t;
    }

    gl = renderer_for(ctx);
    if (!gl)
        return;
    /* Each context has its own texture, so the upload is keyed on the bezel's
     * generation rather than done once globally. */
    if (ov_gl_image_gen(gl) != bezel->gen)
        ov_gl_set_image(gl, f->have_bezel ? bezel->img.rgba : NULL,
                        bezel->img.w, bezel->img.h, bezel->gen);
    ov_frame_build(&dl, f, w, h);
    ov_gl_draw(gl, &dl, w, h, g_rotation);
}

static void draw_overlay(EGLDisplay dpy, EGLSurface surf)
{
    ov_frame frame;
    EGLContext ctx;
    EGLint w = 0, h = 0;

    if (!frame_wanted(&frame))
        return;

    ctx = real_eglGetCurrentContext();
    if (!ctx)
        return;
    if (!real_eglQuerySurface(dpy, surf, EGL_WIDTH, &w) ||
        !real_eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) || w <= 0 || h <= 0)
        return;

    g_in_hook = 1;
    if (!g_seen_egl) {
        g_seen_egl = 1;
        ov_log("drawing through eglSwapBuffers, %dx%d", w, h);
    }
    draw_gl(ctx, w, h, &frame);
    g_in_hook = 0;
}

/* SDL_Renderer applications -- ScummVM, SDLPoP and the other ports that never
 * touch GL themselves -- present through SDL_RenderPresent.  SDL calls its own
 * SDL_GL_SwapWindow from in there, but that call is bound inside libSDL2 and
 * never reaches our interposed one, so this is the only place to catch them.
 *
 * With an accelerated renderer there is a live GL context at this point, so the
 * overlay is drawn the usual way; SDL's own queued geometry has to be flushed
 * first, or the flush inside SDL_RenderPresent would land on top of us. */
static void draw_overlay_sdl_renderer(void *renderer)
{
    static sdl_output_size_fn output_size;
    static sdl_flush_fn flush;
    static int resolved;
    ov_frame frame;
    EGLContext ctx;
    int w = 0, h = 0;

    if (!frame_wanted(&frame))
        return;
    if (!resolved) {
        output_size = (sdl_output_size_fn)sdl_sym("SDL_GetRendererOutputSize");
        flush = (sdl_flush_fn)sdl_sym("SDL_RenderFlush");   /* SDL >= 2.0.10 */
        resolved = 1;
    }
    if (!real_eglGetCurrentContext)
        return;
    ctx = real_eglGetCurrentContext();
    if (!ctx) {
        /* A software renderer has no GL context to draw into; that path needs
         * a rasteriser of its own, which is not written yet. */
        if (!g_seen_sdl_sw) {
            g_seen_sdl_sw = 1;
            ov_log("SDL_RenderPresent with no GL context: software renderer, "
                   "overlay not drawn");
        }
        return;
    }
    if (!output_size || output_size(renderer, &w, &h) != 0 || w <= 0 || h <= 0) {
        if (!g_seen_sdl_sw) {
            g_seen_sdl_sw = 1;
            ov_log("SDL_RenderPresent: no renderer size, overlay not drawn");
        }
        return;
    }

    g_in_hook = 1;
    if (getenv("OV_NO_SDL_FLUSH"))
        flush = NULL;               /* bisecting: is the flush the problem? */
    if (!g_seen_sdl_ren) {
        g_seen_sdl_ren = 1;
        ov_log("drawing through SDL_RenderPresent, %dx%d%s", w, h,
               flush ? "" : " (no SDL_RenderFlush: SDL is older than 2.0.10)");
    }
    if (flush)
        flush(renderer);
    draw_gl(ctx, w, h, &frame);
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
    if (!g_seen_sdl_gl) {
        g_seen_sdl_gl = 1;
        ov_log("SDL_GL_SwapWindow hooked");
    }
    draw_overlay_current();
    if (real)
        real(window);
}

OV_EXPORT void SDL_RenderPresent(void *renderer)
{
    static sdl_present_fn real;

    if (!real)
        real = (sdl_present_fn)sdl_sym("SDL_RenderPresent");
    draw_overlay_sdl_renderer(renderer);
    if (real)
        real(renderer);
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
