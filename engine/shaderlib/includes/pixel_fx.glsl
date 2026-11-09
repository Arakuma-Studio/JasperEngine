// Pixel FX helpers (docs/specs/S48-pixel-fx.md D1, D4), shared by library:sprite and the screen
// transition. Every function is integer or plain float maths on texel coordinates (no texture, no
// derivative), so all backends agree, and engine/core/pixelart/PixelFxKey.cpp mirrors it on the
// CPU (S49 spawns dissolve embers where the GPU burns). The ordered-dither table is S43's
// includes/bayer.glsl, the family's only one.

#ifndef PIXEL_FX_GLSL
#define PIXEL_FX_GLSL

// PCG2D (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020): 32-bit, highp uint,
// which GLSL ES 3.00 guarantees in fragment shaders
uvec2 pfxPcg2d(uvec2 v){
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v = v ^ (v >> 16u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v = v ^ (v >> 16u);
    return v;
}

// a 32-bit hash of a texel (non-negative coordinates) and a seed
uint pfxHash(uvec2 texel, uint seed){
    return pfxPcg2d(texel ^ uvec2(seed, seed * 2654435769u)).x;
}

// the hash as [0, 1): the top 24 bits, exact in a float on every backend
float pfxHash01(uvec2 texel, uint seed){
    return float(pfxHash(texel, seed) >> 8u) * (1.0 / 16777216.0);
}

// Value noise in [0, 1): the hashes at whole cells of `cell` texels, bilinear between them. p is
// a texel-space position (non-negative), usually a texel centre.
float pfxValueNoise(vec2 p, float cell, uint seed){
    vec2 q = p / max(cell, 1.0);
    vec2 i = floor(q);
    vec2 f = q - i;
    uvec2 c = uvec2(i);
    float a = pfxHash01(c, seed);
    float b = pfxHash01(c + uvec2(1u, 0u), seed);
    float d = pfxHash01(c + uvec2(0u, 1u), seed);
    float e = pfxHash01(c + uvec2(1u, 1u), seed);
    float lo = a + (b - a) * f.x;
    float hi = d + (e - d) * f.x;
    return lo + (hi - lo) * f.y;
}

#endif
