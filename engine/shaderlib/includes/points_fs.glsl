// The colour of a point for library points shaders (@shader points), as the built-in
// points.frag computes it: the point texture (HAS_TEXTURE) at the point's rotated
// coordinate inside its sprite-sheet rect (HAS_TEXTURERECT), times the point's colour.
// Declares the inputs includes/points_vs.glsl writes and u_pointsTexture, which the
// engine binds by name. Fragment stage.

#ifndef POINTS_FS_GLSL
#define POINTS_FS_GLSL

#include "includes/srgb.glsl"

#ifdef HAS_TEXTURE
    in float v_pointrotation;
#endif
#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 v_color;
#endif
#ifdef HAS_TEXTURERECT
    in vec4 v_texturerect;
#endif

#ifdef HAS_TEXTURE
    uniform texture2D u_pointsTexture;
    uniform sampler u_points_smp;
#endif

// the point's own coordinate, 0..1 across the square, rotated with the point
vec2 pointCoord(){
    vec2 coord = gl_PointCoord;
    #ifdef HAS_TEXTURE
        if (v_pointrotation != 0.0){
            float c = cos(v_pointrotation);
            float s = sin(v_pointrotation);
            coord -= 0.5;
            coord = vec2(c * coord.x + s * coord.y, c * coord.y - s * coord.x) + 0.5;
        }
    #endif
    return coord;
}

// the point's colour (linear, straight alpha)
vec4 pointVertexColor(){
    vec4 color = vec4(1.0);
    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb = v_color;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC4
        color = v_color;
    #endif
    return color;
}

// the texture at a point coordinate, inside the point's rect (linear, straight alpha);
// white without a texture
vec4 pointTextureColor(vec2 coord){
    #ifdef HAS_TEXTURE
        #ifdef HAS_TEXTURERECT
            coord = coord * v_texturerect.zw + v_texturerect.xy;
        #endif
        return sRGBToLinear(texture(sampler2D(u_pointsTexture, u_points_smp), coord));
    #else
        return vec4(1.0);
    #endif
}

#endif
