// Shared vertex path of library points shaders (@shader points: Points and the particle
// systems drawing into them): a <name>.vert includes only this. Mirrors the built-in
// points.vert: each point is a screen-facing square of a_pointsize world units.
//
// Outputs, by the variant's defines (both stages see them):
//   v_pointrotation  HAS_TEXTURE: the point's rotation in radians
//   v_color          HAS_VERTEX_COLOR_VEC3 / VEC4: the point's colour
//   v_texturerect    HAS_TEXTURERECT: the point's sprite-sheet rect (xy origin, zw size)

uniform u_vs_pointsParams {
    mat4 mvpMatrix;
    float pointScale;
} pointsParams;

in vec3 a_position;
in float a_pointsize;

#ifdef HAS_TEXTURE
    in float a_pointrotation;
    out float v_pointrotation;
#endif

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 a_color;
    out vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 a_color;
    out vec4 v_color;
#endif

#ifdef HAS_TEXTURERECT
    in vec4 a_texturerect;
    out vec4 v_texturerect;
#endif

void main() {
    #ifdef HAS_TEXTURE
        v_pointrotation = a_pointrotation;
    #endif

    #if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
        v_color = a_color;
    #endif

    #ifdef HAS_TEXTURERECT
        v_texturerect = a_texturerect;
    #endif

    gl_Position = pointsParams.mvpMatrix * vec4(a_position, 1.0);

    // gl_Position.w is the eye-space depth for perspective and exactly 1.0 for
    // ortho/UI, so this single expression gives world-space sizing in both cases.
    gl_PointSize = max(a_pointsize * pointsParams.pointScale / gl_Position.w, 1.0);

    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
