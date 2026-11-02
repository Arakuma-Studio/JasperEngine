// Adapted from OpenEngine (MIT), Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// (Engine/Modules/Rendering/Shaders/Includes/terrain_material_albedo.glsl: the colour-slotted
// hex lattice), after Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(2), 2022.
//
// Hex tiling: a tiled texture sampled on three copies, each rotated and shifted by its hex
// tile's hash, blended by the barycentrics of the hex lattice's dual triangle the point is in.
// The grid a plain repeat shows at a distance is gone; a sample costs three taps. Every tap is
// a textureGrad with the analytic gradients (the tile's rotation applied to the UV's own), so
// there is no mip seam where a tap's tile changes.
//
// The block between HEX_LATTICE_BEGIN and HEX_LATTICE_END is pure lattice math (no samplers,
// no globals, f-suffixed literals): engine/tests/core/shader/HexTilingTest.cpp compiles it as
// C++ and checks it. The hash is an integer hash (PCG2D), so tiles far from the origin (large
// ids on fp32 mobile GPUs) hash as well as the ones next to it.

#ifndef HEX_TILING_GLSL
#define HEX_TILING_GLSL

#include "includes/normal_map.glsl"

precision highp float;
precision highp int;

// HEX_LATTICE_BEGIN
// the exponent of the barycentric weights (how narrow the blend between the copies is)
const float HEX_WEIGHT_EXPONENT = 7.0f;
// 2 sqrt(3): hex tiles per texture repeat (the paper's density)
const float HEX_DENSITY = 3.4641016151f;

// One triangle of the hex lattice's dual: its three vertex ids and their barycentric
// weights. Slot k holds the vertex whose colour (hexColor) is k: crossing an edge of the
// triangle replaces one vertex by another of the same colour, and that slot's weight is zero
// on the edge, so a tap never jumps where it has weight.
struct HexSimplex{
    vec3 w;
    ivec2 v0;
    ivec2 v1;
    ivec2 v2;
};

// The 3-colouring of the lattice: the vertices of a triangle have consecutive (x - y), so
// (x - y) mod 3 gives each triangle one vertex of each colour. Taken in uint, where the
// subtraction and % are defined for every pair of ids: a borrow (x < y) wraps by 2^32, which
// is 1 mod 3, and is taken back out.
int hexColor(ivec2 v){
    int m = int((uint(v.x) - uint(v.y)) % 3u) - (v.x < v.y ? 1 : 0);
    return m < 0 ? 2 : m;
}

// The triangle of the dual lattice holding a UV, its vertices slotted by colour
HexSimplex hexSimplex(vec2 uv){
    vec2 st = uv * HEX_DENSITY;
    // into the skewed lattice basis (columns (1, 0) and (-1/sqrt(3), 2/sqrt(3)))
    vec2 skewed = vec2(st.x - 0.57735027f * st.y, 1.15470054f * st.y);
    ivec2 base = ivec2(int(floor(skewed.x)), int(floor(skewed.y)));
    vec2 f = vec2(skewed.x - floor(skewed.x), skewed.y - floor(skewed.y));
    float tz = 1.0f - f.x - f.y;
    float s = tz < 0.0f ? 1.0f : 0.0f;
    float s2 = 2.0f * s - 1.0f;
    vec3 bw = vec3(-tz * s2, s - f.y * s2, s - f.x * s2);
    int si = int(s);
    ivec2 p0 = ivec2(base.x + si, base.y + si);
    ivec2 p1 = ivec2(base.x + si, base.y + 1 - si);
    ivec2 p2 = ivec2(base.x + 1 - si, base.y + si);
    // p0's colour decides the others: p1 and p2 are one less and one more, in the order the
    // triangle's parity gives
    int k0 = hexColor(p0);
    int kNext = k0 == 2 ? 0 : k0 + 1;
    int kPrev = k0 == 0 ? 2 : k0 - 1;
    int k1 = si == 0 ? kPrev : kNext;
    HexSimplex simplex;
    simplex.v0 = k0 == 0 ? p0 : (k1 == 0 ? p1 : p2);
    simplex.v1 = k0 == 1 ? p0 : (k1 == 1 ? p1 : p2);
    simplex.v2 = k0 == 2 ? p0 : (k1 == 2 ? p1 : p2);
    simplex.w = vec3(k0 == 0 ? bw.x : (k1 == 0 ? bw.y : bw.z),
                     k0 == 1 ? bw.x : (k1 == 1 ? bw.y : bw.z),
                     k0 == 2 ? bw.x : (k1 == 2 ? bw.y : bw.z));
    return simplex;
}

// PCG2D (Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 2020)
uvec2 hexHash(ivec2 v){
    uvec2 h = uvec2(uint(v.x), uint(v.y)) * 1664525u + 1013904223u;
    h.x += h.y * 1664525u;
    h.y += h.x * 1664525u;
    h = h ^ (h >> 16u);
    h.x += h.y * 1664525u;
    h.y += h.x * 1664525u;
    h = h ^ (h >> 16u);
    return h;
}

// 0..1 from the top 24 bits (exact in a float)
float hexUnit(uint h){
    return float(h >> 8u) * (1.0f / 16777216.0f);
}

// A tile's rotation (cos, sin): up to half a turn either way, times rotation (0..1)
vec2 hexRotation(ivec2 v, float rotation){
    uvec2 h = hexHash(ivec2(v.y, v.x));
    float angle = (hexUnit(h.x) * 2.0f - 1.0f) * 3.14159265f * rotation;
    return vec2(cos(angle), sin(angle));
}

// A tile's shift, 0..1 in each direction
vec2 hexOffset(ivec2 v){
    uvec2 h = hexHash(v);
    return vec2(hexUnit(h.x), hexUnit(h.y));
}

// A hex lattice vertex's UV (the inverse of the skew above)
vec2 hexCenter(ivec2 v){
    return vec2(float(v.x) + 0.5f * float(v.y), 0.8660254038f * float(v.y)) * (1.0f / HEX_DENSITY);
}

// (cos, sin) rotation of a 2D vector, and its inverse
vec2 hexRotate(vec2 p, vec2 cs){
    return vec2(cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y);
}
vec2 hexRotateBack(vec2 p, vec2 cs){
    return vec2(cs.x * p.x + cs.y * p.y, -cs.y * p.x + cs.x * p.y);
}

// Where a tile samples a UV: rotated about the tile's vertex and shifted, continuous in the UV
// for a fixed tile (so its gradient is the UV's, rotated)
vec2 hexTapUV(vec2 uv, ivec2 v, vec2 cs){
    vec2 center = hexCenter(v);
    return hexRotate(uv - center, cs) + center + hexOffset(v);
}

// The barycentrics to HEX_WEIGHT_EXPONENT: what hexWeights scales by luminance (a function of
// the taps alone, so the maps and layers that share the taps share it)
vec3 hexWeightPowers(vec3 barycentrics){
    return vec3(pow(max(barycentrics.x, 0.0f), HEX_WEIGHT_EXPONENT),
                pow(max(barycentrics.y, 0.0f), HEX_WEIGHT_EXPONENT),
                pow(max(barycentrics.z, 0.0f), HEX_WEIGHT_EXPONENT));
}

// hexWeights with the barycentrics' powers given (hexWeightPowers of the same barycentrics)
vec3 hexWeightsOfPowers(vec3 barycentrics, vec3 b, vec3 luminance, float contrast){
    vec3 l = vec3(1.0f - contrast + contrast * luminance.x,
                  1.0f - contrast + contrast * luminance.y,
                  1.0f - contrast + contrast * luminance.z);
    vec3 w = vec3(b.x * l.x, b.y * l.y, b.z * l.z);
    float sum = w.x + w.y + w.z;
    return sum > 1e-20f ? w * (1.0f / sum) : barycentrics;
}

// The blend weights of three taps: the barycentrics to HEX_WEIGHT_EXPONENT, each scaled by
// its tap's luminance as far as contrast (0..1) says (a bright tap wins over a dark one, which
// keeps the texture's contrast where they cross), normalized
vec3 hexWeights(vec3 barycentrics, vec3 luminance, float contrast){
    return hexWeightsOfPowers(barycentrics, hexWeightPowers(barycentrics), luminance, contrast);
}
// HEX_LATTICE_END

// The three taps of one UV, set up once per fragment (every hex-sampled texture of a
// material shares them)
struct HexTaps{
    vec2 uv0;
    vec2 uv1;
    vec2 uv2;
    vec2 cs0; // each tile's rotation (cos, sin)
    vec2 cs1;
    vec2 cs2;
    vec2 dx0; // each tap's gradients
    vec2 dy0;
    vec2 dx1;
    vec2 dy1;
    vec2 dx2;
    vec2 dy2;
    vec3 barycentrics;
};

// uv's gradients are taken by the caller in uniform control flow
HexTaps hexSetup(vec2 uv, vec2 dx, vec2 dy, float rotation){
    HexSimplex simplex = hexSimplex(uv);
    HexTaps taps;
    taps.cs0 = hexRotation(simplex.v0, rotation);
    taps.cs1 = hexRotation(simplex.v1, rotation);
    taps.cs2 = hexRotation(simplex.v2, rotation);
    taps.uv0 = hexTapUV(uv, simplex.v0, taps.cs0);
    taps.uv1 = hexTapUV(uv, simplex.v1, taps.cs1);
    taps.uv2 = hexTapUV(uv, simplex.v2, taps.cs2);
    taps.dx0 = hexRotate(dx, taps.cs0);
    taps.dy0 = hexRotate(dy, taps.cs0);
    taps.dx1 = hexRotate(dx, taps.cs1);
    taps.dy1 = hexRotate(dy, taps.cs1);
    taps.dx2 = hexRotate(dx, taps.cs2);
    taps.dy2 = hexRotate(dy, taps.cs2);
    taps.barycentrics = simplex.w;
    return taps;
}

float hexLuminance(vec3 color){
    return dot(color, vec3(0.299, 0.587, 0.114));
}

// A tangent-space normal sampled on a rotated tile, back in the UV's own frame
vec3 hexNormalBack(vec3 tangentNormal, vec2 cs){
    return vec3(hexRotateBack(tangentNormal.xy, cs), tangentNormal.z);
}

// the three taps of a 2D texture (taps: a HexTaps; tex, smp: the texture and its sampler)
#define HEX_TAP0(tex, smp, taps) textureGrad(sampler2D(tex, smp), (taps).uv0, (taps).dx0, (taps).dy0)
#define HEX_TAP1(tex, smp, taps) textureGrad(sampler2D(tex, smp), (taps).uv1, (taps).dx1, (taps).dy1)
#define HEX_TAP2(tex, smp, taps) textureGrad(sampler2D(tex, smp), (taps).uv2, (taps).dx2, (taps).dy2)
// blended by weights (from hexWeights)
#define HEX_SAMPLE(tex, smp, taps, weights) \
    (HEX_TAP0(tex, smp, taps) * (weights).x + HEX_TAP1(tex, smp, taps) * (weights).y + HEX_TAP2(tex, smp, taps) * (weights).z)
// a tangent-space normal map (xyz in -1..1), each tap's normal turned back by its tile
#define HEX_SAMPLE_NORMAL(tex, smp, taps, weights) \
    (hexNormalBack(decodeNormalMap(HEX_TAP0(tex, smp, taps)), (taps).cs0) * (weights).x + \
     hexNormalBack(decodeNormalMap(HEX_TAP1(tex, smp, taps)), (taps).cs1) * (weights).y + \
     hexNormalBack(decodeNormalMap(HEX_TAP2(tex, smp, taps)), (taps).cs2) * (weights).z)

#endif
