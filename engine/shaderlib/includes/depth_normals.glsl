// The view-space normal a depth pre-pass writes beside its packed depth for Cavity (S92 R2;
// DEPTH 'Cvn', DEPTH_NORMALS), in a second RGBA8 attachment: octahedral (includes/octahedral.glsl)
// at 12 bits a component, rg their high 8 bits, b their low 4 bits (x high nibble, y low), a 1
// where a program wrote a normal and 0 where none did: the clear (background) or a program
// that cannot tell its surface's normal, where Cavity rebuilds one from the depth.
//
// The vertex stage (depth.vert, includes/surface_depth_vs.glsl) writes v_depthNormal: xyz the
// view-space normal, w 1 (0: it has none). The shader compiler wraps every 'Cvn' fragment
// stage (ShaderVariantCompiler: the program's own main runs, then depthNormalsOutput), so a
// fork of depth.frag needs no change; it defines DEPTH_NORMALS_VARYING when the vertex stage
// writes v_depthNormal (a vertex fork made before it does not: its pixels say none).

#ifndef DEPTH_NORMALS_GLSL
#define DEPTH_NORMALS_GLSL

#include "includes/octahedral.glsl"

vec4 depthNormalsEncode(vec3 n){
    vec2 code = floor(octEncode(normalize(n)) * 4095.0 + 0.5);
    vec2 high = floor(code / 16.0);
    vec2 low = code - high * 16.0;
    return vec4(high / 255.0, (low.x * 16.0 + low.y) / 255.0, 1.0);
}

// xyz the normal, w 1 where one was written (0: none)
vec4 depthNormalsDecode(vec4 texel){
    vec2 high = floor(texel.rg * 255.0 + 0.5);
    float low = floor(texel.b * 255.0 + 0.5);
    float lowX = floor(low / 16.0);
    vec2 code = high * 16.0 + vec2(lowX, low - lowX * 16.0);
    return vec4(octDecode(code / 4095.0), step(0.5, texel.a));
}

#endif // DEPTH_NORMALS_GLSL
