// Fragment stage of the library depth programs: writes the packed depth the depth passes
// expect (as depth.frag). A shader that alpha clips defines, before including,
//
//   #define SURFACE_DEPTH_DISCARD
//
// and after it
//
//   bool surfaceDepthDiscard(vec2 uv, vec4 color){ ... true drops the fragment ... }
//
// with uv the first UV set (SURFACE_UV1, else 0) and color the vertex colour
// (SURFACE_COLOR, else white). Its vertex stage then defines SURFACE_DEPTH_VARYINGS
// (includes/surface_depth_vs.glsl), so the varyings the hook reads exist.

out vec4 frag_color;
in vec2 v_projZW;

#ifdef SURFACE_DEPTH_DISCARD
    #ifdef SURFACE_UV1
        in vec2 v_uv1;
    #endif
    #ifdef SURFACE_COLOR
        in vec4 v_color;
    #endif

    bool surfaceDepthDiscard(vec2 uv, vec4 color);
#endif

#include "includes/depth_util.glsl"

#include "includes/foliage_dither.glsl"

void main() {
    #ifdef USE_FOLIAGE_DITHER
        foliageDitherDiscard();
    #endif
    #ifdef SURFACE_DEPTH_DISCARD
        vec2 uv = vec2(0.0);
        vec4 color = vec4(1.0);
        #ifdef SURFACE_UV1
            uv = v_uv1;
        #endif
        #ifdef SURFACE_COLOR
            color = v_color;
        #endif
        if (surfaceDepthDiscard(uv, color)){
            discard;
        }
    #endif

    frag_color = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);
}
