/* Vulkan overlay vertex shader.  The quad comes from gl_VertexIndex, so there
 * is no vertex buffer to bind: every command is four vertices and a push
 * constant block. */
#version 450

layout(push_constant) uniform Push {
    vec4  rect;         /* x, y, w, h, in pixels */
    vec4  uv;           /* u, v, du, dv */
    vec2  screen;
    vec2  half_size;
    vec4  fill;
    vec4  stroke;
    float radius;
    float border;
    int   mode;         /* 0 rounded rect, 1 glyph, 2 image */
    int   rot;          /* quarter turns clockwise */
} pc;

layout(location = 0) out vec2 v_local;
layout(location = 1) out vec2 v_uv;

void main()
{
    vec2 unit = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
    vec2 p = pc.rect.xy + unit * pc.rect.zw;

    v_local = (unit - 0.5) * pc.rect.zw;
    v_uv = pc.uv.xy + unit * pc.uv.zw;

    /* Vulkan clip space already has +y downwards, which is the direction the
     * layout uses, so unlike the GL shader there is no flip here. */
    vec2 ndc = vec2(p.x / pc.screen.x * 2.0 - 1.0,
                    p.y / pc.screen.y * 2.0 - 1.0);
    if (pc.rot == 1)      ndc = vec2(-ndc.y, ndc.x);
    else if (pc.rot == 2) ndc = -ndc;
    else if (pc.rot == 3) ndc = vec2(ndc.y, -ndc.x);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
