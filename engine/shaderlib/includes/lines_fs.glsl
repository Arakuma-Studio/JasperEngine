// The colour of a line for library lines shaders (@shader lines), as the built-in
// lines.frag computes it: the vertex colour (the segment colours, or the Line Renderer's
// colour gradient / the Trail Renderer's over the trail, linear) and, for a ribbon
// (LINE_RIBBON, a width > 0), the texture coordinate along it. Declares the inputs
// includes/lines_vs.glsl writes. Fragment stage.
//
// lineUV(): u along the line by the component's texture mode (Stretch 0..1 over the length,
// Tile 1 per 1 / textureScale.x world units, per segment...), v 0 on one side and 1 on the
// other (times textureScale); (0, 0) on a hairline, which has no texture coordinates.

#ifndef LINES_FS_GLSL
#define LINES_FS_GLSL

#include "includes/srgb.glsl"

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 v_color;
#endif
#ifdef LINE_RIBBON
    in vec2 v_uv1;
#endif

// the line's colour (linear, straight alpha)
vec4 lineVertexColor(){
    vec4 color = vec4(1.0);
    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb = v_color;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC4
        color = v_color;
    #endif
    return color;
}

vec2 lineUV(){
    #ifdef LINE_RIBBON
        return v_uv1;
    #else
        return vec2(0.0);
    #endif
}

#endif
