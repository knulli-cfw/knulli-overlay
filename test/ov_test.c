/* Host-side visual check: renders the overlay over a fake "game" frame into an
 * offscreen EGL pbuffer and writes a PPM.  Build with `make test`.
 *
 *   ov_test <w> <h> <volume|-1> <out.ppm> [battery%] [charging] [notification]
 *
 * $OV_BEZEL adds a decoration underneath, $OV_TEST_RADIOS=wifi,bluetooth the
 * radio icons in the status pill, and $OV_TEST_CLOCK="09:41" the clock. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_bezel.h"
#include "ov_gl.h"
#include "ov_layout.h"
#include "ov_state.h"

static void write_ppm(const char *path, int w, int h, const unsigned char *rgba)
{
    FILE *f = fopen(path, "wb");
    int y, x;

    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--)            /* GL reads bottom-up */
        for (x = 0; x < w; x++)
            fwrite(rgba + (y * w + x) * 4, 1, 3, f);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 640;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    int value = argc > 3 ? atoi(argv[3]) : 60;
    const char *out = argc > 4 ? argv[4] : "overlay.ppm";
    int battery = argc > 5 ? atoi(argv[5]) : OV_BATTERY_NONE;
    int charging = argc > 6 ? atoi(argv[6]) : 0;
    const char *text = argc > 7 ? argv[7] : NULL;
    const char *radios = getenv("OV_TEST_RADIOS");

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint pb_attr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLSurface surf;
    EGLContext ctx;
    EGLint n;

    ov_snapshot snap;
    ov_style style;
    ov_drawlist dl;
    ov_bezel bezel;
    ov_gl *gl;
    unsigned char *pixels;
    int x, y;

    ov_snapshot_defaults(&snap);
    snap.kind = value < 0 ? OV_KIND_NONE : OV_KIND_VOLUME;
    snap.value = value;
    snap.battery = battery;
    snap.battery_flags = charging ? OV_FLAG_CHARGING : 0;
    /* $OV_TEST_RADIOS=wifi,bluetooth fakes what knulli.conf would have said. */
    if (radios) {
        if (strstr(radios, "wifi"))
            snap.battery_flags |= OV_FLAG_WIFI;
        if (strstr(radios, "bt") || strstr(radios, "bluetooth"))
            snap.battery_flags |= OV_FLAG_BLUETOOTH;
        if (strstr(radios, "nopercent"))
            snap.battery_flags |= OV_FLAG_NO_PERCENT;
    }
    if (text) {
        strncpy(snap.text, text, sizeof(snap.text) - 1);
        snap.text[sizeof(snap.text) - 1] = '\0';
    }

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL) ||
        !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "EGL setup failed: 0x%x\n", eglGetError());
        return 1;
    }
    surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "EGL context failed: 0x%x\n", eglGetError());
        return 1;
    }

    /* a coarse checkerboard stands in for the game underneath */
    glViewport(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    for (y = 0; y < h; y += 32) {
        for (x = 0; x < w; x += 32) {
            float c = ((x / 32 + y / 32) & 1) ? 0.30f : 0.55f;
            glScissor(x, y, 32, 32);
            glClearColor(c, c * 0.8f, c * 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glDisable(GL_SCISSOR_TEST);

    ov_style_defaults(&style);
    /* The library formats the real time; here it is given, so a render is
     * reproducible. */
    style.clock_text = getenv("OV_TEST_CLOCK");
    ov_gl_set_egl_getproc((void *)eglGetProcAddress);
    gl = ov_gl_create(NULL);
    if (!gl) {
        fprintf(stderr, "renderer init failed\n");
        return 1;
    }
    ov_layout_reset(&dl);
    /* $OV_BEZEL points at a decoration to composite underneath, the same way
     * the injected library picks one up from hud.config. */
    ov_bezel_init(&bezel);
    if (ov_bezel_poll(&bezel)) {
        float bx, by, bw, bh;

        ov_gl_set_image(gl, bezel.img.rgba, bezel.img.w, bezel.img.h,
                        bezel.gen);
        ov_bezel_rect(&bezel, w, h, &bx, &by, &bw, &bh);
        ov_layout_add_image(&dl, bx, by, bw, bh, bezel.opacity, bezel.hole);
        printf("bezel: %s %dx%d at %.0f,%.0f %.0fx%.0f alpha %.2f\n",
               bezel.path, bezel.img.w, bezel.img.h, bx, by, bw, bh,
               bezel.opacity);
    }
    ov_layout_build(&dl, &snap, &style, w, h);
    printf("drawlist: %d commands\n", dl.count);
    ov_gl_draw(gl, &dl, w, h, 0);

    pixels = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    write_ppm(out, w, h, pixels);
    free(pixels);
    ov_gl_destroy(gl);
    return 0;
}
