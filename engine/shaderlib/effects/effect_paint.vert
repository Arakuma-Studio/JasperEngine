#version 450

// HTML UI effects, paint form (phase 5): the element's geometry (the RmlUi vertex layout of
// htmlui.vert) mapped by `transform` (projection * element transform). v_uv runs 0..1 over the
// effect area (beyond it for @geometry bounds); v_color is the premultiplied vertex colour,
// its alpha the element's draw opacity.
uniform u_vs_effectVertex {
    mat4 transform;
    vec4 translate;     // xy: the RmlUi translation
} vtx;

in vec2 a_position;
in vec4 a_color;
in vec2 a_texcoord1;

// varyings in the same order in both stages: their locations follow declaration order
out vec2 v_uv;
out vec4 v_color;

void main() {
    v_uv = a_texcoord1;
    v_color = a_color;
    gl_Position = vtx.transform * vec4(a_position + vtx.translate.xy, 0.0, 1.0);
    // no depth test: keep z inside every backend's clip range (GL [-w,w], D3D/Metal/Vulkan [0,w])
    gl_Position.z = 0.5 * gl_Position.w;
}
