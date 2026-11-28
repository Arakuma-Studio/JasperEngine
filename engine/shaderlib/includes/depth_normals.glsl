// The world-space normal a depth pre-pass writes beside its packed depth for Cavity (S100 R2;
// DEPTH 'Cvn', DEPTH_NORMALS), in a second RGBA8 attachment: octahedral (includes/octahedral.glsl)
// at 12 bits a component, rg their high 8 bits, b their low 4 bits (x high nibble, y low), a 1
// where a program wrote a normal and 0 where none did: the clear (background) or a program
// that cannot tell its surface's normal, where Cavity rebuilds one from the depth.
//
// The vertex stage (depth.vert, includes/surface_depth_vs.glsl) writes v_depthNormal: xyz the
// world-space normal, from the object block alone (a depth program takes no camera block for
// it), w 1 (0: it has none). The shader compiler wraps every 'Cvn' fragment stage
// (ShaderVariantCompiler: the program's own main runs, then the normals output), so a fork of
// depth.frag needs no change; it defines DEPTH_NORMALS_VARYING when the vertex stage writes
// v_depthNormal (a vertex fork made before it does not: its pixels say none).

#ifndef DEPTH_NORMALS_GLSL
#define DEPTH_NORMALS_GLSL

#include "includes/octahedral.glsl"

vec4 depthNormalsEncode(vec3 n){
    vec2 code = floor(octEncode(normalize(n)) * 4095.0 + 0.5);
    vec2 codeHigh = floor(code / 16.0);
    vec2 codeLow = code - codeHigh * 16.0;
    return vec4(codeHigh / 255.0, (codeLow.x * 16.0 + codeLow.y) / 255.0, 1.0);
}

// xyz the normal, w 1 where one was written (0: none)
vec4 depthNormalsDecode(vec4 texel){
    vec2 codeHigh = floor(texel.rg * 255.0 + 0.5);
    float codeLow = floor(texel.b * 255.0 + 0.5);
    float codeLowX = floor(codeLow / 16.0);
    vec2 code = codeHigh * 16.0 + vec2(codeLowX, codeLow - codeLowX * 16.0);
    return vec4(octDecode(code / 4095.0), step(0.5, texel.a));
}

#endif // DEPTH_NORMALS_GLSL
