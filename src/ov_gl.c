#define _GNU_SOURCE
#include "ov_gl.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_atlas.h"

/* --- minimal GL declarations -------------------------------------------- */
/* Declared here rather than pulled from <GLES2/gl2.h> so the library builds
 * and runs regardless of which GL headers/implementation the board ships. */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef signed long int GLintptr;
typedef signed long int GLsizeiptr;
typedef unsigned int GLbitfield;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLE_STRIP 0x0005
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_DITHER 0x0BD0
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_FUNC_ADD 0x8006
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_ALPHA 0x1906
#define GL_RGBA 0x1908
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_VERSION 0x1F02
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_NO_ERROR 0

struct gl_fns {
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    GLboolean (*IsEnabled)(GLenum);
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*BlendEquationSeparate)(GLenum, GLenum);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*GetBooleanv)(GLenum, GLboolean *);
    const GLubyte *(*GetString)(GLenum);
    GLenum (*GetError)(void);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*DepthMask)(GLboolean);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*PixelStorei)(GLenum, GLint);
    void (*ActiveTexture)(GLenum);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                       GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (*DeleteBuffers)(GLsizei, const GLuint *);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*DeleteProgram)(GLuint);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    GLint (*GetAttribLocation)(GLuint, const GLchar *);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform1f)(GLint, GLfloat);
    void (*Uniform2f)(GLint, GLfloat, GLfloat);
    void (*Uniform4fv)(GLint, GLsizei, const GLfloat *);
    void (*EnableVertexAttribArray)(GLuint);
    void (*DisableVertexAttribArray)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                const void *);
    void (*GetVertexAttribiv)(GLuint, GLenum, GLint *);
    void (*GetVertexAttribPointerv)(GLuint, GLenum, void **);
    /* optional, core-profile only */
    void (*GenVertexArrays)(GLsizei, GLuint *);
    void (*BindVertexArray)(GLuint);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
};

/* --- shaders ------------------------------------------------------------ */

static const char *VS_BODY =
"IN vec2 a_unit;\n"
"uniform vec4 u_rect;\n"        /* x, y, w, h in pixels, y down from the top */
"uniform vec2 u_screen;\n"
"uniform vec4 u_uv;\n"
"uniform int u_rot;\n"
"OUT vec2 v_local;\n"
"OUT vec2 v_uv;\n"
"void main() {\n"
"    vec2 p = u_rect.xy + a_unit * u_rect.zw;\n"
"    v_local = (a_unit - 0.5) * u_rect.zw;\n"
"    v_uv = u_uv.xy + a_unit * u_uv.zw;\n"
"    vec2 ndc = vec2(p.x / u_screen.x * 2.0 - 1.0,\n"
"                    1.0 - p.y / u_screen.y * 2.0);\n"
"    if (u_rot == 1)      ndc = vec2(-ndc.y, ndc.x);\n"
"    else if (u_rot == 2) ndc = -ndc;\n"
"    else if (u_rot == 3) ndc = vec2(ndc.y, -ndc.x);\n"
"    gl_Position = vec4(ndc, 0.0, 1.0);\n"
"}\n";

/* Rounded rectangles are drawn from their signed distance field, which keeps
 * the corners and the 1px border crisp at any panel size. */
static const char *FS_BODY =
"IN vec2 v_local;\n"
"IN vec2 v_uv;\n"
"uniform vec2 u_half;\n"
"uniform float u_radius;\n"
"uniform float u_border;\n"
"uniform vec4 u_fill;\n"
"uniform vec4 u_stroke;\n"
"uniform int u_mode;\n"
"uniform sampler2D u_tex;\n"
"void main() {\n"
"    if (u_mode == 1) {\n"
"        FRAGCOLOR = vec4(u_fill.rgb, u_fill.a * TEXALPHA(u_tex, v_uv));\n"
"        return;\n"
"    }\n"
"    if (u_mode == 2) {\n"
"        FRAGCOLOR = TEXRGBA(u_tex, v_uv) * u_fill;\n"
"        return;\n"
"    }\n"
"    vec2 q = abs(v_local) - u_half + vec2(u_radius);\n"
"    float d = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - u_radius;\n"
"    vec4 col = u_fill;\n"
"    if (u_border > 0.0) {\n"
"        float inner = clamp(0.5 - (d + u_border), 0.0, 1.0);\n"
"        col = mix(u_stroke, u_fill, inner);\n"
"    }\n"
"    FRAGCOLOR = vec4(col.rgb, col.a * clamp(0.5 - d, 0.0, 1.0));\n"
"}\n";

/* GLSL dialects we may land in.  The device case is ES 1.00; the others exist
 * so the same overlay is usable on a desktop build of the same emulator. */
static const char *VS_PRE_ES100 = "#define IN attribute\n#define OUT varying\n";
static const char *FS_PRE_ES100 =
"#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n"
"#else\nprecision mediump float;\n#endif\n"
"#define IN varying\n"
"#define FRAGCOLOR gl_FragColor\n"
"#define TEXALPHA(t, uv) texture2D(t, uv).a\n"
"#define TEXRGBA(t, uv) texture2D(t, uv)\n";
static const char *VS_PRE_GL110 = "#define IN attribute\n#define OUT varying\n";
static const char *FS_PRE_GL110 =
"#define IN varying\n"
"#define FRAGCOLOR gl_FragColor\n"
"#define TEXALPHA(t, uv) texture2D(t, uv).a\n"
"#define TEXRGBA(t, uv) texture2D(t, uv)\n";
static const char *VS_PRE_GL150 =
"#version 150\n#define IN in\n#define OUT out\n";
static const char *FS_PRE_GL150 =
"#version 150\n#define IN in\n"
"out vec4 ov_frag;\n#define FRAGCOLOR ov_frag\n"
"#define TEXALPHA(t, uv) texture(t, uv).r\n"
"#define TEXRGBA(t, uv) texture(t, uv)\n";

/* --- renderer ----------------------------------------------------------- */

struct ov_gl {
    struct gl_fns gl;
    GLuint prog, vbo, tex, img_tex, vao;
    int img_w, img_h;
    unsigned img_gen;
    GLint a_unit;
    GLint u_rect, u_screen, u_uv, u_rot;
    GLint u_half, u_radius, u_border, u_fill, u_stroke, u_mode, u_tex;
    int use_red;      /* single channel texture is GL_RED rather than GL_ALPHA */
    /* $OV_NO_DRAW keeps every state change and skips the geometry, which tells
     * a problem with what we draw from one with the state we leave behind. */
    int no_draw;
    int have_vao;
    int ok;
};

/* Everything we touch and therefore have to hand back exactly as we found it. */
struct gl_saved {
    GLint program, array_buffer, element_buffer, fbo, vao;
    GLint active_texture, texture0;
    GLint viewport[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint blend_eq_rgb, blend_eq_alpha;
    GLboolean blend, depth, cull, scissor, stencil, dither;
    GLboolean color_mask[4], depth_mask;
    struct {
        GLint enabled, size, stride, type, normalized, buffer;
        void *pointer;
    } attr;
};

static void *(*g_egl_getproc)(const char *);

void ov_gl_set_egl_getproc(void *fn)
{
    g_egl_getproc = (void *(*)(const char *))fn;
}

void *ov_gl_default_getproc(const char *name)
{
    /* Both are needed: the GLES ones for drawing, libEGL for the surface
     * queries in the swap hooks. */
    static const char *const libs[] = {
        "libGLESv2.so.2", "libGLESv2.so", "libEGL.so.1", "libEGL.so", NULL
    };
    static void *handles[4];
    static int opened;
    void *p = dlsym(RTLD_DEFAULT, name);
    int i;

    /* Most drivers have the entry points in the global scope already.
     * Otherwise ask EGL -- though a PowerVR blob answers NULL for the core
     * GLES functions -- and only then open the libraries ourselves, which is
     * what it takes when the app dlopen()s them with RTLD_LOCAL, as SDL2
     * does. */
    if (!p && g_egl_getproc)
        p = g_egl_getproc(name);
    if (!p) {
        if (!opened) {
            for (i = 0; libs[i]; i++)
                handles[i] = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
            opened = 1;
        }
        for (i = 0; libs[i] && !p; i++)
            if (handles[i])
                p = dlsym(handles[i], name);
    }
    return p;
}

static void *resolve(ov_getproc getproc, const char *name)
{
    return getproc ? getproc(name) : ov_gl_default_getproc(name);
}

#define LOAD(field, name) \
    do { g->gl.field = (void *)resolve(getproc, name); \
         if (!g->gl.field) { missing = name; } } while (0)

static int load_fns(struct ov_gl *g, ov_getproc getproc)
{
    const char *missing = NULL;

    LOAD(Enable, "glEnable");
    LOAD(Disable, "glDisable");
    LOAD(IsEnabled, "glIsEnabled");
    LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(BlendEquationSeparate, "glBlendEquationSeparate");
    LOAD(GetIntegerv, "glGetIntegerv");
    LOAD(GetBooleanv, "glGetBooleanv");
    LOAD(GetString, "glGetString");
    LOAD(GetError, "glGetError");
    LOAD(Viewport, "glViewport");
    LOAD(ColorMask, "glColorMask");
    LOAD(DepthMask, "glDepthMask");
    LOAD(DrawArrays, "glDrawArrays");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(PixelStorei, "glPixelStorei");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(GenTextures, "glGenTextures");
    LOAD(BindTexture, "glBindTexture");
    LOAD(TexImage2D, "glTexImage2D");
    LOAD(TexParameteri, "glTexParameteri");
    LOAD(DeleteTextures, "glDeleteTextures");
    LOAD(GenBuffers, "glGenBuffers");
    LOAD(BindBuffer, "glBindBuffer");
    LOAD(BufferData, "glBufferData");
    LOAD(DeleteBuffers, "glDeleteBuffers");
    LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource");
    LOAD(CompileShader, "glCompileShader");
    LOAD(GetShaderiv, "glGetShaderiv");
    LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(DeleteShader, "glDeleteShader");
    LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader");
    LOAD(LinkProgram, "glLinkProgram");
    LOAD(GetProgramiv, "glGetProgramiv");
    LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD(DeleteProgram, "glDeleteProgram");
    LOAD(UseProgram, "glUseProgram");
    LOAD(GetUniformLocation, "glGetUniformLocation");
    LOAD(GetAttribLocation, "glGetAttribLocation");
    LOAD(Uniform1i, "glUniform1i");
    LOAD(Uniform1f, "glUniform1f");
    LOAD(Uniform2f, "glUniform2f");
    LOAD(Uniform4fv, "glUniform4fv");
    LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD(VertexAttribPointer, "glVertexAttribPointer");
    LOAD(GetVertexAttribiv, "glGetVertexAttribiv");
    LOAD(GetVertexAttribPointerv, "glGetVertexAttribPointerv");

    /* optional */
    g->gl.GenVertexArrays = (void *)resolve(getproc, "glGenVertexArrays");
    g->gl.BindVertexArray = (void *)resolve(getproc, "glBindVertexArray");
    g->gl.DeleteVertexArrays = (void *)resolve(getproc, "glDeleteVertexArrays");
    g->have_vao = g->gl.GenVertexArrays && g->gl.BindVertexArray &&
                  g->gl.DeleteVertexArrays;
    /* A plain GLES2 driver has no VAOs at all; $OV_NO_VAO forces that path on
     * a driver that does, to check the global-state save/restore. */
    if (getenv("OV_NO_VAO"))
        g->have_vao = 0;

    if (missing) {
        fprintf(stderr, "knulli-overlay: missing GL entry point %s\n", missing);
        return 0;
    }
    return 1;
}
#undef LOAD

static GLuint compile(struct ov_gl *g, GLenum type, const char *pre,
                      const char *body)
{
    const char *src[2] = { pre, body };
    GLuint sh = g->gl.CreateShader(type);
    GLint ok = 0;

    if (!sh)
        return 0;
    g->gl.ShaderSource(sh, 2, src, NULL);
    g->gl.CompileShader(sh);
    g->gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        g->gl.GetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "knulli-overlay: shader compile failed: %s\n", log);
        g->gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Picks the GLSL dialect from the GL version string. */
static void pick_dialect(struct ov_gl *g, const char **vs_pre,
                         const char **fs_pre)
{
    const char *ver = (const char *)g->gl.GetString(GL_VERSION);

    if (ver && strstr(ver, "OpenGL ES")) {
        *vs_pre = VS_PRE_ES100;
        *fs_pre = FS_PRE_ES100;
        g->use_red = 0;
        return;
    }
    if (ver && ver[0] >= '3') {         /* desktop GL 3.x+: assume core */
        *vs_pre = VS_PRE_GL150;
        *fs_pre = FS_PRE_GL150;
        g->use_red = 1;
        return;
    }
    *vs_pre = VS_PRE_GL110;
    *fs_pre = FS_PRE_GL110;
    g->use_red = 0;
}

static int build_program(struct ov_gl *g)
{
    static const GLfloat quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const char *vs_pre, *fs_pre;
    GLuint vs, fs;
    GLint linked = 0;

    pick_dialect(g, &vs_pre, &fs_pre);

    vs = compile(g, GL_VERTEX_SHADER, vs_pre, VS_BODY);
    fs = compile(g, GL_FRAGMENT_SHADER, fs_pre, FS_BODY);
    if (!vs || !fs)
        return 0;

    g->prog = g->gl.CreateProgram();
    g->gl.AttachShader(g->prog, vs);
    g->gl.AttachShader(g->prog, fs);
    g->gl.LinkProgram(g->prog);
    g->gl.DeleteShader(vs);
    g->gl.DeleteShader(fs);
    g->gl.GetProgramiv(g->prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = { 0 };
        g->gl.GetProgramInfoLog(g->prog, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "knulli-overlay: program link failed: %s\n", log);
        return 0;
    }

    g->a_unit   = g->gl.GetAttribLocation(g->prog, "a_unit");
    g->u_rect   = g->gl.GetUniformLocation(g->prog, "u_rect");
    g->u_screen = g->gl.GetUniformLocation(g->prog, "u_screen");
    g->u_uv     = g->gl.GetUniformLocation(g->prog, "u_uv");
    g->u_rot    = g->gl.GetUniformLocation(g->prog, "u_rot");
    g->u_half   = g->gl.GetUniformLocation(g->prog, "u_half");
    g->u_radius = g->gl.GetUniformLocation(g->prog, "u_radius");
    g->u_border = g->gl.GetUniformLocation(g->prog, "u_border");
    g->u_fill   = g->gl.GetUniformLocation(g->prog, "u_fill");
    g->u_stroke = g->gl.GetUniformLocation(g->prog, "u_stroke");
    g->u_mode   = g->gl.GetUniformLocation(g->prog, "u_mode");
    g->u_tex    = g->gl.GetUniformLocation(g->prog, "u_tex");
    if (g->a_unit < 0)
        return 0;

    g->gl.GenBuffers(1, &g->vbo);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    return 1;
}

static int build_texture(struct ov_gl *g)
{
    GLint internal = g->use_red ? GL_R8 : GL_ALPHA;
    GLenum format = g->use_red ? GL_RED : GL_ALPHA;

    g->gl.GenTextures(1, &g->tex);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->tex);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);   /* put back by the caller */
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, internal, OV_ATLAS_W, OV_ATLAS_H, 0,
                     format, GL_UNSIGNED_BYTE, ov_atlas_pixels);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return 1;
}

/* GL_RGBA8 does not exist on plain GLES2, where the internal format has to be
 * the same GL_RGBA as the data. */
void ov_gl_set_image(ov_gl *g, const void *rgba, int w, int h, unsigned gen)
{
    GLint prev_unit = GL_TEXTURE0, prev_tex = 0, prev_align = 4;

    if (!g || !g->ok)
        return;
    if (!rgba || w <= 0 || h <= 0) {
        if (g->img_tex) {
            g->gl.DeleteTextures(1, &g->img_tex);
            g->img_tex = 0;
        }
        g->img_w = g->img_h = 0;
        g->img_gen = gen;
        return;
    }
    /* This runs outside ov_gl_draw()'s save/restore -- the injector uploads
     * when the bezel changes, not per frame -- so put back everything it
     * touches before returning. */
    g->gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    g->gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);

    if (!g->img_tex)
        g->gl.GenTextures(1, &g->img_tex);
    g->gl.BindTexture(GL_TEXTURE_2D, g->img_tex);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    g->gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    g->gl.ActiveTexture((GLenum)prev_unit);
    g->img_w = w;
    g->img_h = h;
    g->img_gen = gen;
    while (g->gl.GetError() != GL_NO_ERROR)
        ;               /* an out-of-memory upload must not poison the app */
}

unsigned ov_gl_image_gen(const ov_gl *g)
{
    return g ? g->img_gen : 0;
}

/* Building the program and the atlas binds a buffer and a texture, and sets
 * the unpack alignment.  This happens once, in the middle of somebody else's
 * frame, so all of it has to be put back:  SDL's renderer keeps a software
 * cache of what it thinks is bound and skips redundant binds, so a texture
 * left bound here is a texture SDL draws its whole scene with. */
struct create_saved {
    GLint active_texture, texture0, array_buffer, unpack_alignment, vao;
};

static void save_for_create(struct ov_gl *g, struct create_saved *s)
{
    s->vao = 0;
    if (g->have_vao)
        g->gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    g->gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture0);
    g->gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    g->gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
}

static void restore_after_create(struct ov_gl *g, const struct create_saved *s)
{
    if (g->have_vao)
        g->gl.BindVertexArray((GLuint)s->vao);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    g->gl.BindTexture(GL_TEXTURE_2D, (GLuint)s->texture0);
    g->gl.ActiveTexture((GLenum)s->active_texture);
}

ov_gl *ov_gl_create(ov_getproc getproc)
{
    struct ov_gl *g = calloc(1, sizeof(*g));   /* getproc NULL: built-in */
    struct create_saved saved;
    int ok;

    if (!g)
        return NULL;
    if (!load_fns(g, getproc)) {
        free(g);
        return NULL;
    }

    save_for_create(g, &saved);
    if (g->have_vao) {
        /* A core profile refuses to draw without one; harmless elsewhere. */
        g->gl.GenVertexArrays(1, &g->vao);
        g->gl.BindVertexArray(g->vao);
    }
    ok = build_program(g) && build_texture(g);
    restore_after_create(g, &saved);
    if (!ok) {
        ov_gl_destroy(g);
        return NULL;
    }

    while (g->gl.GetError() != GL_NO_ERROR)
        ;               /* swallow anything our setup produced */
    g->no_draw = getenv("OV_NO_DRAW") != NULL;
    g->ok = 1;
    return g;
}

void ov_gl_destroy(ov_gl *g)
{
    if (!g)
        return;
    if (g->vbo)
        g->gl.DeleteBuffers(1, &g->vbo);
    if (g->tex)
        g->gl.DeleteTextures(1, &g->tex);
    if (g->img_tex)
        g->gl.DeleteTextures(1, &g->img_tex);
    if (g->prog)
        g->gl.DeleteProgram(g->prog);
    if (g->vao && g->have_vao)
        g->gl.DeleteVertexArrays(1, &g->vao);
    free(g);
}

static void save_state(struct ov_gl *g, struct gl_saved *s)
{
    const struct gl_fns *gl = &g->gl;

    gl->GetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    gl->GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    gl->GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->element_buffer);
    gl->GetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    gl->GetIntegerv(GL_VIEWPORT, s->viewport);
    gl->GetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    gl->GetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    gl->GetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    gl->GetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    gl->GetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    gl->GetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
    gl->GetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    gl->GetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    s->blend = gl->IsEnabled(GL_BLEND);
    s->depth = gl->IsEnabled(GL_DEPTH_TEST);
    s->cull = gl->IsEnabled(GL_CULL_FACE);
    s->scissor = gl->IsEnabled(GL_SCISSOR_TEST);
    s->stencil = gl->IsEnabled(GL_STENCIL_TEST);
    s->dither = gl->IsEnabled(GL_DITHER);
    s->vao = 0;
    if (g->have_vao)
        gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);

    /* our vertex attribute slot, as the app left it */
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture0);
    if (g->have_vao)
        return;                 /* the rest lives in the app's VAO */
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr.enabled);
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s->attr.size);
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s->attr.stride);
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s->attr.type);
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s->attr.normalized);
    gl->GetVertexAttribiv((GLuint)g->a_unit, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s->attr.buffer);
    gl->GetVertexAttribPointerv((GLuint)g->a_unit, 0x8645 /* ARRAY_POINTER */, &s->attr.pointer);
}

static void restore_state(struct ov_gl *g, const struct gl_saved *s)
{
    const struct gl_fns *gl = &g->gl;

    if (g->have_vao) {
        /* Our attribute setup went into our own VAO, so rebinding the app's is
         * all it takes; on plain GLES2 the state is global and must be put
         * back by hand. */
        gl->BindVertexArray((GLuint)s->vao);
    } else {
        gl->BindBuffer(GL_ARRAY_BUFFER, (GLuint)s->attr.buffer);
        if (s->attr.enabled)
            gl->EnableVertexAttribArray((GLuint)g->a_unit);
        else
            gl->DisableVertexAttribArray((GLuint)g->a_unit);
        gl->VertexAttribPointer((GLuint)g->a_unit, s->attr.size,
                                (GLenum)s->attr.type,
                                (GLboolean)s->attr.normalized, s->attr.stride,
                                s->attr.pointer);
    }
    gl->BindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    gl->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s->element_buffer);
    gl->BindTexture(GL_TEXTURE_2D, (GLuint)s->texture0);
    gl->ActiveTexture((GLenum)s->active_texture);
    gl->UseProgram((GLuint)s->program);
    gl->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
    gl->Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    gl->ColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2],
                  s->color_mask[3]);
    gl->DepthMask(s->depth_mask);
    gl->BlendFuncSeparate((GLenum)s->blend_src_rgb, (GLenum)s->blend_dst_rgb,
                          (GLenum)s->blend_src_alpha, (GLenum)s->blend_dst_alpha);
    gl->BlendEquationSeparate((GLenum)s->blend_eq_rgb, (GLenum)s->blend_eq_alpha);
    (s->blend ? gl->Enable : gl->Disable)(GL_BLEND);
    (s->depth ? gl->Enable : gl->Disable)(GL_DEPTH_TEST);
    (s->cull ? gl->Enable : gl->Disable)(GL_CULL_FACE);
    (s->scissor ? gl->Enable : gl->Disable)(GL_SCISSOR_TEST);
    (s->stencil ? gl->Enable : gl->Disable)(GL_STENCIL_TEST);
    (s->dither ? gl->Enable : gl->Disable)(GL_DITHER);
}

void ov_gl_draw(ov_gl *g, const ov_drawlist *dl, int w, int h, int rotation)
{
    struct gl_saved saved;
    const struct gl_fns *gl;
    GLuint bound;
    int i;
    int fb_w = w, fb_h = h;

    if (!g || !g->ok || !dl || dl->count == 0 || w <= 0 || h <= 0)
        return;
    gl = &g->gl;

    save_state(g, &saved);

    if (rotation & 1) {             /* the framebuffer is the other way round */
        fb_w = h;
        fb_h = w;
    }

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->Viewport(0, 0, fb_w, fb_h);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_CULL_FACE);
    gl->Disable(GL_SCISSOR_TEST);
    gl->Disable(GL_STENCIL_TEST);
    gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->DepthMask(GL_FALSE);
    gl->Enable(GL_BLEND);
    gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl->BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                          GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (g->have_vao)
        gl->BindVertexArray(g->vao);
    gl->UseProgram(g->prog);
    gl->BindBuffer(GL_ARRAY_BUFFER, g->vbo);
    gl->EnableVertexAttribArray((GLuint)g->a_unit);
    gl->VertexAttribPointer((GLuint)g->a_unit, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, g->tex);
    bound = g->tex;
    gl->Uniform1i(g->u_tex, 0);
    gl->Uniform2f(g->u_screen, (GLfloat)w, (GLfloat)h);
    gl->Uniform1i(g->u_rot, rotation & 3);

    for (i = 0; i < dl->count && !g->no_draw; i++) {
        const ov_cmd *c = &dl->cmd[i];
        GLfloat rect[4] = { c->x, c->y, c->w, c->h };
        GLfloat uv[4] = { 0, 0, 0, 0 };
        GLuint want;
        int mode;

        if (c->image && !g->img_tex)
            continue;               /* nothing uploaded yet */
        if (c->image) {
            uv[0] = c->uv[0];
            uv[1] = c->uv[1];
            uv[2] = c->uv[2];
            uv[3] = c->uv[3];
            mode = 2;
        } else {
            mode = c->glyph >= 0 ? 1 : 0;
        }
        want = c->image ? g->img_tex : g->tex;
        if (want != bound) {
            gl->BindTexture(GL_TEXTURE_2D, want);
            bound = want;
        }
        if (c->glyph >= 0) {
            const ov_glyph *gy = &ov_atlas_glyphs[c->glyph];
            uv[0] = (GLfloat)gy->x / OV_ATLAS_W;
            uv[1] = (GLfloat)gy->y / OV_ATLAS_H;
            uv[2] = (GLfloat)gy->w / OV_ATLAS_W;
            uv[3] = (GLfloat)gy->h / OV_ATLAS_H;
        }
        gl->Uniform1i(g->u_mode, mode);
        gl->Uniform4fv(g->u_rect, 1, rect);
        gl->Uniform4fv(g->u_uv, 1, uv);
        gl->Uniform2f(g->u_half, c->w * 0.5f, c->h * 0.5f);
        gl->Uniform1f(g->u_radius, c->radius);
        gl->Uniform1f(g->u_border, c->border);
        gl->Uniform4fv(g->u_fill, 1, c->fill);
        gl->Uniform4fv(g->u_stroke, 1, c->stroke);
        gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    restore_state(g, &saved);
    while (gl->GetError() != GL_NO_ERROR)
        ;               /* do not leak our errors into the app's error queue */
}
