#version 450

// HTML UI geometry (RmlUi) and the document composite: positions in document pixels mapped
// by `transform` (projection * element transform), premultiplied sRGB colors passed through.
// Batched runs (S16) bake the translation into the positions and pass a zero one.
uniform u_vs_htmluiParams {
    mat4 transform;
    vec4 translation;   // xy: RenderGeometry translation, z unused, w: 1 keeps the depth (World Space)
} htmluiParams;

in vec2 a_position;
in vec4 a_color;
#ifdef HAS_TEXTURE
    in vec2 a_texcoord1;
#endif

// varyings in the same order in both stages: their locations follow declaration order (the
// document position last: the gradient's fragment stage does not read it)
out vec4 v_color;
#ifdef HAS_TEXTURE
    out vec2 v_uv1;
#endif
out vec2 v_docPos;      // document px before the element transform: the analytic clip's space

void main() {
    v_color = a_color;
    #ifdef HAS_TEXTURE
        v_uv1 = a_texcoord1;
    #endif
    v_docPos = a_position + htmluiParams.translation.xy;
    gl_Position = htmluiParams.transform * vec4(v_docPos, 0.0, 1.0);
    if (htmluiParams.translation.w > 0.5) {
        // a World Space document's quad (S93 R36): the scene's depth, tested against it
        #ifdef IS_VULKAN
            // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
            gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
        #endif
    } else {
        // no depth test: keep z inside every backend's clip range (GL [-w,w], D3D/Metal/Vulkan [0,w])
        gl_Position.z = 0.5 * gl_Position.w;
    }
}
