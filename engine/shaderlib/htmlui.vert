#version 450

// HTML UI geometry (RmlUi) and the document composite: positions in document pixels mapped
// by `transform` (projection * element transform), premultiplied sRGB colors passed through.
uniform u_vs_htmluiParams {
    mat4 transform;
    vec4 translation;   // xy: RenderGeometry translation, zw unused
} htmluiParams;

in vec2 a_position;
in vec4 a_color;
#ifdef HAS_TEXTURE
    in vec2 a_texcoord1;
#endif

// varyings in the same order in both stages: their locations follow declaration order
out vec4 v_color;
#ifdef HAS_TEXTURE
    out vec2 v_uv1;
#endif

void main() {
    v_color = a_color;
    #ifdef HAS_TEXTURE
        v_uv1 = a_texcoord1;
    #endif
    gl_Position = htmluiParams.transform * vec4(a_position + htmluiParams.translation.xy, 0.0, 1.0);
    // no depth test: keep z inside every backend's clip range (GL [-w,w], D3D/Metal/Vulkan [0,w])
    gl_Position.z = 0.5 * gl_Position.w;
}
