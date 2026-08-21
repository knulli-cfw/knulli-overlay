/* Vulkan overlay fragment shader: the same signed distance field rounded
 * rectangle as the GL one, plus the glyph atlas and the bezel image. */
#version 450

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(set = 0, binding = 1) uniform sampler2D u_image;

layout(push_constant) uniform Push {
    vec4  rect;
    vec4  uv;
    vec2  screen;
    vec2  half_size;
    vec4  fill;
    vec4  stroke;
    float radius;
    float border;
    int   mode;
    int   rot;
} pc;

layout(location = 0) out vec4 frag;

void main()
{
    if (pc.mode == 1) {         /* glyph: the atlas carries coverage only */
        frag = vec4(pc.fill.rgb, pc.fill.a * texture(u_atlas, v_uv).r);
        return;
    }
    if (pc.mode == 2) {         /* bezel */
        frag = texture(u_image, v_uv) * pc.fill;
        return;
    }

    vec2 q = abs(v_local) - pc.half_size + vec2(pc.radius);
    float d = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - pc.radius;
    vec4 col = pc.fill;

    if (pc.border > 0.0) {
        float inner = clamp(0.5 - (d + pc.border), 0.0, 1.0);
        col = mix(pc.stroke, pc.fill, inner);
    }
    frag = vec4(col.rgb, col.a * clamp(0.5 - d, 0.0, 1.0));
}
