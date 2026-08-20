/* Draws an ov_drawlist with OpenGL ES 2 / OpenGL 2.1+.
 *
 * Nothing here links against libGLESv2 or libEGL: every entry point is
 * resolved at run time from the GL that the host process already loaded, so
 * the same .so works on Mali blobs, Mesa and desktop GL alike.
 */
#ifndef OV_GL_H
#define OV_GL_H

#include "ov_layout.h"

typedef void *(*ov_getproc)(const char *name);

typedef struct ov_gl ov_gl;

/* Tells the built-in resolver about the host's eglGetProcAddress; optional,
 * but it is the only way in on drivers that keep GLES out of the global
 * scope. */
void ov_gl_set_egl_getproc(void *fn);

/* The built-in resolver: dlsym(RTLD_DEFAULT), then eglGetProcAddress, then a
 * dlopen() of libGLESv2 as a last resort. */
void *ov_gl_default_getproc(const char *name);

/* Pass NULL for `getproc` to use ov_gl_default_getproc(). */
ov_gl *ov_gl_create(ov_getproc getproc);
void   ov_gl_destroy(ov_gl *g);

/* Hands the renderer the bezel image (straight RGBA8, top row first), which
 * it uploads to a texture of its own; `gen` is the caller's identifier for
 * that image, and ov_gl_image_gen() reports the one currently uploaded so a
 * per-context renderer knows when it has fallen behind.  A NULL image drops
 * the texture. */
void   ov_gl_set_image(ov_gl *g, const void *rgba, int w, int h, unsigned gen);
unsigned ov_gl_image_gen(const ov_gl *g);

/* Draws over the current default framebuffer.  `rotation` is in quarter turns
 * clockwise (0..3) and `w`/`h` are the logical (post-rotation) size. */
void   ov_gl_draw(ov_gl *g, const ov_drawlist *dl, int w, int h, int rotation);

#endif /* OV_GL_H */
