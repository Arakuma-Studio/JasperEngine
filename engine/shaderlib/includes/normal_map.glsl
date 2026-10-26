// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NORMAL_MAP_GLSL
#define NORMAL_MAP_GLSL

// A tangent-space normal from a normal map texel. Normal maps are stored as two channels (BC5,
// EAC RG11, ASTC rg, UASTC RG; RG8 uncompressed): the third component is rebuilt, so every
// platform's format decodes alike, and an RGB map decodes to the same normal. Custom shaders
// sampling a normal map call it too (docs/textures.md).
vec3 decodeNormalMap(vec4 texel){
    vec2 xy = texel.rg * 2.0 - vec2(1.0);
    return vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
}

// Stochastic Tiling's normal taps (includes/stochastic.glsl, included after this file; one
// included before keeps its own reading, as the terrain's array taps do)
#ifdef STOCHASTIC_DECODE_NORMAL
    #undef STOCHASTIC_DECODE_NORMAL
#endif
#define STOCHASTIC_DECODE_NORMAL(texel) decodeNormalMap(texel)

#endif
