/* A stand-in for the game: draws a frame, calls eglSwapBuffers, then dumps the
 * buffer.  Used to exercise the LD_PRELOAD path end to end:
 *
 *   OV_STATE_FILE=/tmp/ov.state ./knulli-overlay volume 60
 *   OV_STATE_FILE=/tmp/ov.state LD_PRELOAD=./libknulli-overlay.so ./ov_app out.ppm
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "app.ppm";
    int w = argc > 2 ? atoi(argv[2]) : 640;
    int h = argc > 3 ? atoi(argv[3]) : 480;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint pb_attr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg;
    EGLSurface surf;
    EGLContext ctx;
    EGLint n;
    unsigned char *px;
    FILE *f;
    int x, y;

    if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
        return fprintf(stderr, "EGL init failed\n"), 1;
    surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!eglMakeCurrent(dpy, surf, surf, ctx))
        return fprintf(stderr, "EGL makecurrent failed\n"), 1;

    /* Leave the GL in a deliberately awkward state so a bad state restore in
     * the overlay shows up as a wrong result below. */
    glViewport(0, 0, w / 2, h / 2);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glClearColor(0.15f, 0.35f, 0.55f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(dpy, surf);

    printf("after swap: viewport=");
    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        printf("%d,%d,%d,%d depth=%d cull=%d blend=%d err=0x%x\n",
               vp[0], vp[1], vp[2], vp[3], glIsEnabled(GL_DEPTH_TEST),
               glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_BLEND), glGetError());
    }

    px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++)
            fwrite(px + (y * w + x) * 4, 1, 3, f);
    fclose(f);
    printf("wrote %s\n", out);
    return 0;
}
