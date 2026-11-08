// Shared vertex path of library UI shaders (@shader ui: Image, Text, Polygon and the
// Image-based widgets): a <name>.vert includes only this. Mirrors the built-in ui.vert.
//
// Outputs, by the variant's defines (both stages see them):
//   v_color          HAS_VERTEX_COLOR_VEC3 / VEC4: the vertex colour (Image, Polygon)
//   v_uv1            HAS_TEXTURE (Image) or HAS_FONTATLAS_TEXTURE (Text): the texture UV
//   v_localPosition  UI_LOCAL_POSITION (defined by the shader): the vertex position in
//                    the element's own space, in UI pixels (an Image spans 0..width,
//                    0..height); for effects laid over the element whatever its texture
//
// Varyings pair up by location, in declaration order: a fragment shader declares
// v_localPosition after including includes/ui_fs.glsl (which declares the others).

uniform u_vs_uiParams {
    mat4 mvpMatrix;
    #ifdef PIXEL_SNAP
        vec4 pixelSnapDraw; // x the draw's snap: 0 none, 1 every vertex, 2 the pivot (S41)
    #endif
} uiParams;

#ifdef PIXEL_SNAP
    #define FRAME_VS
    #include "includes/frame.glsl"
    #include "includes/pixel_snap.glsl"
#endif

in vec3 a_position;

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 a_color;
    out vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 a_color;
    out vec4 v_color;
#endif

#if defined(HAS_TEXTURE) || defined(HAS_FONTATLAS_TEXTURE)
    in vec2 a_texcoord1;
    out vec2 v_uv1;
#endif

#ifdef UI_LOCAL_POSITION
    out vec2 v_localPosition;
#endif

void main() {
    #if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
        v_color = a_color;
    #endif

    #if defined(HAS_TEXTURE) || defined(HAS_FONTATLAS_TEXTURE)
        v_uv1 = a_texcoord1;
    #endif

    #ifdef UI_LOCAL_POSITION
        v_localPosition = a_position.xy;
    #endif

    gl_Position = uiParams.mvpMatrix * vec4(a_position, 1.0);
    #ifdef PIXEL_SNAP
        gl_Position = pixelSnapApply(gl_Position, uiParams.pixelSnapDraw.x, uiParams.mvpMatrix[3]);
    #endif
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
