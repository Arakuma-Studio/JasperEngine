// Stochastic Tiling: runtime anti-repetition sampling of tiled textures (hex-tiling).
//
// A port of Morten S. Mikkelsen, "Practical Real-Time Hex-Tiling", Journal of Computer
// Graphics Techniques 11(2), 2022 (https://jcgt.org/published/0011/03/05/), and of its
// reference code, hextile-demo/hextiling.h (https://github.com/mmikk/hextile-demo),
// MIT License, Copyright (c) 2022 mmikk. The licence text: includes/LICENSE.hextiling.txt
// (listed in NOTICE.md). Changes from the reference: an integer hash (pcg3d) for the
// per-tile offset and rotation instead of the sin hash and the closed-form angle, gradients
// passed in (taken once, of the un-offset UV) and rotated per tile, taps of a negligible
// weight skipped, the cell size in texture repeats, and the weights computed once and shared
// by every map of a material (the luminance-modulated ones from its base colour).
//
// No precomputation: each sampled map costs up to three textureGrad taps. The UV plane is
// covered by a triangle lattice; each lattice vertex is the centre of a hexagonal cell that
// shows the texture with its own random offset (and, with rotation, its own random angle).
// A pixel blends the three cells of the triangle it falls in, by its barycentric weights
// raised to the power gamma (Blend Sharpness), optionally modulated by the luminance of the
// base colour taps (beta 0.6), then sharpened by Perlin's gain curve (Contrast).
//
// The texture must tile (REPEAT wrap): every tap reads it at an arbitrary offset.
//
// ---------------------------------------------------------------- API
//
// Parameters. The user-facing values (inspector, Lua, MCP; the terrain layers and the
// library shaders' STOCHASTIC feature) map to the vec4 the setup takes with
//
//   vec4 stochasticParams(float cellSize, float blendSharpness, float contrast, float rotation)
//     cellSize       > 0: hex cell width in texture repeats (1: one cell spans one repeat)
//     blendSharpness 0..1: gamma = 1 + 15 * blendSharpness (0.4: Mikkelsen's gamma 7)
//     contrast       0..1: Mikkelsen's r = 0.5 + 0.45 * contrast, 0 is no gain
//     rotation       0..1: random rotation per cell, the fraction of +-180 degrees
//   returns (1 / cellSize, gamma, gain exponent k = -log2(1 - r), rotation * PI)
//
// Setup, once per fragment and UV set: the lattice, the three taps (coordinates, gradients,
// rotation) and the geometric weights. `seed` decorrelates surfaces sharing a UV (0 is fine).
//
//   StochasticTiling stochasticSetup(vec2 uv, vec2 dUVdx, vec2 dUVdy, vec4 params, uint seed)
//     dUVdx, dUVdy: the screen-space gradients of the un-offset uv (dFdx(uv), dFdy(uv)), taken
//     where the control flow is still uniform. Safe to call anywhere.
//   StochasticTiling stochasticSetup(vec2 uv, vec4 params, uint seed)
//     takes the gradients itself: uniform control flow only.
//
//   st.weights: the blend weights every helper below uses (sum 1, a tap of weight 0 is never
//   fetched). After the setup they are geometric (gamma, gain). The luminance helpers replace
//   them with the luminance-modulated ones; sample the base colour that way FIRST, then every
//   other map of the material with the same StochasticTiling, so they all transition alike.
//
// Sampling (textureGrad: valid in non-uniform control flow, e.g. per terrain layer):
//
//   vec4 stochasticTexture(texture2D tex, sampler smp, StochasticTiling st)
//   vec4 stochasticTextureArray(texture2DArray tex, sampler smp, float layer, StochasticTiling st)
//     the weighted blend of the taps.
//   vec4 stochasticTextureLuminance(texture2D tex, sampler smp, inout StochasticTiling st)
//   vec4 stochasticTextureArrayLuminance(texture2DArray tex, sampler smp, float layer, inout StochasticTiling st)
//     the same, with st.weights first replaced by the weights modulated by the taps' luminance
//     (of the values as stored, as the reference). For the base colour.
//   vec3 stochasticNormal(texture2D tex, sampler smp, StochasticTiling st, float strength, float greenSign)
//   vec3 stochasticNormalArray(texture2DArray tex, sampler smp, float layer, StochasticTiling st, float strength, float greenSign)
//     a tangent-space normal map (decodeNormalMap for the 2D map, so a two-channel cook decodes
//     like its source; rgb * 2 - 1 for the array, built from the source files): the taps become
//     height derivatives, are turned back by their cell's rotation, blended with st.weights and
//     made a unit normal again whose xy are then scaled by strength, as the plain
//     `n = decodeNormalMap(texture(...)); n.xy *= strength;` (which it equals for one tap).
//     greenSign is the way the caller's tangent frame reads green against +v, which decides
//     the way a rotation turns the slopes back (a tap must read as the texel a turned copy of
//     the map would hold):
//       STOCHASTIC_NORMAL_GL (-1): green read against +v. Lit, the library shaders and the 2D
//         meshes (Sprite, Polygon, Tilemap) (their bitangent points up the image while images
//         load top row first).
//       STOCHASTIC_NORMAL_DX (+1): green read along +v. The terrain layers, whose whiteout
//         blend takes green as +v on the flat and side projections.
//     Rotation 0 makes it irrelevant (the taps are not turned).
//
// Lower level, for maps decoded another way (two-channel normals, packed channels):
//
//   vec4 stochasticGrad(texture2D tex, sampler smp, StochasticTiling st, int tap)     tap 0..2
//   vec2 stochasticTapUV(StochasticTiling st, int tap), vec4 stochasticTapGrad(..., int tap)
//     a tap's coordinate and gradients (xy d/dx, zw d/dy) for textureGrad
//   vec4 stochasticBlend(StochasticTiling st, vec4 c0, vec4 c1, vec4 c2)
//   void stochasticLuminanceWeights(inout StochasticTiling st, vec3 c0, vec3 c1, vec3 c2)
//   vec3 stochasticBlendNormals(StochasticTiling st, vec3 n0, vec3 n1, vec3 n2, float strength, float greenSign)
//     n0..n2 tangent normals as decoded from each tap
//
// Taps of weight 0 may be left unfetched (pass anything): check st.weights[tap] > 0.0.

#ifndef STOCHASTIC_GLSL
#define STOCHASTIC_GLSL

// how stochasticNormal reads a tap: includes/normal_map.glsl, included before this file where a
// normal map is sampled, makes it decodeNormalMap (a two-channel cook decodes like its source)
#ifndef STOCHASTIC_DECODE_NORMAL
    #define STOCHASTIC_DECODE_NORMAL(texel) texel.rgb * 2.0 - 1.0
#endif

// Below this a tap's weight is dropped and the others renormalised: at most a 1/255 change
// of the blend, and most pixels then fetch one or two taps instead of three
const float STOCHASTIC_TAP_EPSILON = 0.002;
const float STOCHASTIC_PI = 3.14159265;
// greenSign of the normal helpers
const float STOCHASTIC_NORMAL_GL = -1.0;
const float STOCHASTIC_NORMAL_DX = 1.0;

struct StochasticTiling{
    vec2 uv0;
    vec2 uv1;
    vec2 uv2;
    vec4 grad0;     // the tap's gradients, rotated with it: xy d/dx, zw d/dy
    vec4 grad1;
    vec4 grad2;
    vec2 rot0;      // cos, sin of the tap's rotation
    vec2 rot1;
    vec2 rot2;
    vec3 lattice;   // barycentric weights raised to gamma, normalised (before the gain)
    vec3 mask;      // 1 for the taps that are fetched
    vec3 weights;   // the blend weights (sum 1)
    float gain;     // gain exponent k (1: none)
};

vec4 stochasticParams(float cellSize, float blendSharpness, float contrast, float rotation){
    float r = 0.5 + 0.45 * clamp(contrast, 0.0, 1.0);
    return vec4(1.0 / max(cellSize, 0.0001),
                1.0 + 15.0 * clamp(blendSharpness, 0.0, 1.0),
                -log2(1.0 - r),
                clamp(rotation, 0.0, 1.0) * STOCHASTIC_PI);
}

// pcg3d (Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 2020)
uvec3 stochasticHash(uvec3 v){
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// Perlin and Hoffert's gain on each weight, renormalised (reference Gain3)
vec3 stochasticGain(vec3 x, float k){
    vec3 s = 2.0 * step(vec3(0.5), x);
    vec3 m = 2.0 * (vec3(1.0) - s);
    vec3 res = 0.5 * s + 0.25 * m * pow(max(vec3(0.0), s + x * m), vec3(k));
    return res / (res.x + res.y + res.z);
}

vec2 stochasticRotate(vec2 v, vec2 cs){
    return vec2(cs.x * v.x - cs.y * v.y, cs.y * v.x + cs.x * v.y);
}

vec2 stochasticRotateBack(vec2 v, vec2 cs){
    return vec2(cs.x * v.x + cs.y * v.y, cs.x * v.y - cs.y * v.x);
}

// One cell's tap: its random offset and angle, rotated about the cell centre
void stochasticCell(vec2 uv, vec2 dUVdx, vec2 dUVdy, ivec2 vertex, vec4 params, uint seed,
                    out vec2 tapUV, out vec4 tapGrad, out vec2 tapRot){
    uvec3 h = stochasticHash(uvec3(uvec2(vertex), seed));
    vec3 random = vec3(h >> 8u) * (1.0 / 16777216.0);

    // the centre in UV: the skewed lattice vertex mapped back, divided by the lattice scale
    vec2 centre = vec2(float(vertex.x) + 0.5 * float(vertex.y), 0.8660254 * float(vertex.y)) / params.x;

    tapRot = vec2(1.0, 0.0);
    if (params.w > 0.0){
        float angle = (random.z * 2.0 - 1.0) * params.w;
        tapRot = vec2(cos(angle), sin(angle));
    }

    // wrapped by whole repeats (REPEAT reads the same), so large UVs keep their precision
    tapUV = stochasticRotate(uv - centre, tapRot) + fract(centre + random.xy);
    tapGrad = vec4(stochasticRotate(dUVdx, tapRot), stochasticRotate(dUVdy, tapRot));
}

// Drops the taps the mask leaves out and renormalises
vec3 stochasticMasked(vec3 w, vec3 mask){
    w *= mask;
    return w / max(w.x + w.y + w.z, 1e-6);
}

StochasticTiling stochasticSetup(vec2 uv, vec2 dUVdx, vec2 dUVdy, vec4 params, uint seed){
    StochasticTiling st;

    // skew into the triangle lattice (vertex spacing 1 / params.x in UV)
    vec2 p = uv * params.x;
    vec2 skewed = vec2(p.x - 0.57735027 * p.y, 1.15470054 * p.y);
    vec2 cell = floor(skewed);
    ivec2 base = ivec2(cell);
    vec3 t = vec3(skewed - cell, 0.0);
    t.z = 1.0 - t.x - t.y;

    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    vec3 w = max(vec3(-t.z * s2, s - t.y * s2, s - t.x * s2), vec3(0.0));

    ivec2 si = ivec2(int(s));
    stochasticCell(uv, dUVdx, dUVdy, base + si, params, seed, st.uv0, st.grad0, st.rot0);
    stochasticCell(uv, dUVdx, dUVdy, base + ivec2(si.x, 1 - si.y), params, seed, st.uv1, st.grad1, st.rot1);
    stochasticCell(uv, dUVdx, dUVdy, base + ivec2(1 - si.x, si.y), params, seed, st.uv2, st.grad2, st.rot2);

    vec3 W = pow(w, vec3(params.y));
    st.lattice = W / max(W.x + W.y + W.z, 1e-30);
    st.gain = params.z;

    vec3 geometric = (st.gain != 1.0) ? stochasticGain(st.lattice, st.gain) : st.lattice;
    st.mask = step(vec3(STOCHASTIC_TAP_EPSILON), geometric);
    st.weights = stochasticMasked(geometric, st.mask);
    return st;
}

StochasticTiling stochasticSetup(vec2 uv, vec4 params, uint seed){
    return stochasticSetup(uv, dFdx(uv), dFdy(uv), params, seed);
}

vec2 stochasticTapUV(StochasticTiling st, int tap){
    return (tap == 0) ? st.uv0 : ((tap == 1) ? st.uv1 : st.uv2);
}

vec4 stochasticTapGrad(StochasticTiling st, int tap){
    return (tap == 0) ? st.grad0 : ((tap == 1) ? st.grad1 : st.grad2);
}

vec4 stochasticGrad(texture2D tex, sampler smp, StochasticTiling st, int tap){
    vec4 g = stochasticTapGrad(st, tap);
    return textureGrad(sampler2D(tex, smp), stochasticTapUV(st, tap), g.xy, g.zw);
}

vec4 stochasticGradArray(texture2DArray tex, sampler smp, float layer, StochasticTiling st, int tap){
    vec4 g = stochasticTapGrad(st, tap);
    return textureGrad(sampler2DArray(tex, smp), vec3(stochasticTapUV(st, tap), layer), g.xy, g.zw);
}

// Mikkelsen's colour weights: the gamma-shaped lattice weights times
// mix(1, luminance, beta = 0.6), normalised, through the gain
void stochasticLuminanceWeights(inout StochasticTiling st, vec3 c0, vec3 c1, vec3 c2){
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    vec3 Dw = mix(vec3(1.0), vec3(dot(c0, luma), dot(c1, luma), dot(c2, luma)), 0.6);
    vec3 W = Dw * st.lattice * st.mask;
    W /= max(W.x + W.y + W.z, 1e-30);
    if (st.gain != 1.0){
        W = stochasticGain(W, st.gain);
    }
    st.weights = stochasticMasked(W, st.mask);
}

vec4 stochasticBlend(StochasticTiling st, vec4 c0, vec4 c1, vec4 c2){
    return c0 * st.weights.x + c1 * st.weights.y + c2 * st.weights.z;
}

vec4 stochasticTexture(texture2D tex, sampler smp, StochasticTiling st){
    vec4 color = vec4(0.0);
    if (st.weights.x > 0.0) color += stochasticGrad(tex, smp, st, 0) * st.weights.x;
    if (st.weights.y > 0.0) color += stochasticGrad(tex, smp, st, 1) * st.weights.y;
    if (st.weights.z > 0.0) color += stochasticGrad(tex, smp, st, 2) * st.weights.z;
    return color;
}

vec4 stochasticTextureArray(texture2DArray tex, sampler smp, float layer, StochasticTiling st){
    vec4 color = vec4(0.0);
    if (st.weights.x > 0.0) color += stochasticGradArray(tex, smp, layer, st, 0) * st.weights.x;
    if (st.weights.y > 0.0) color += stochasticGradArray(tex, smp, layer, st, 1) * st.weights.y;
    if (st.weights.z > 0.0) color += stochasticGradArray(tex, smp, layer, st, 2) * st.weights.z;
    return color;
}

vec4 stochasticTextureLuminance(texture2D tex, sampler smp, inout StochasticTiling st){
    vec4 c0 = vec4(0.0);
    vec4 c1 = vec4(0.0);
    vec4 c2 = vec4(0.0);
    if (st.mask.x > 0.0) c0 = stochasticGrad(tex, smp, st, 0);
    if (st.mask.y > 0.0) c1 = stochasticGrad(tex, smp, st, 1);
    if (st.mask.z > 0.0) c2 = stochasticGrad(tex, smp, st, 2);
    stochasticLuminanceWeights(st, c0.rgb, c1.rgb, c2.rgb);
    return stochasticBlend(st, c0, c1, c2);
}

vec4 stochasticTextureArrayLuminance(texture2DArray tex, sampler smp, float layer, inout StochasticTiling st){
    vec4 c0 = vec4(0.0);
    vec4 c1 = vec4(0.0);
    vec4 c2 = vec4(0.0);
    if (st.mask.x > 0.0) c0 = stochasticGradArray(tex, smp, layer, st, 0);
    if (st.mask.y > 0.0) c1 = stochasticGradArray(tex, smp, layer, st, 1);
    if (st.mask.z > 0.0) c2 = stochasticGradArray(tex, smp, layer, st, 2);
    stochasticLuminanceWeights(st, c0.rgb, c1.rgb, c2.rgb);
    return stochasticBlend(st, c0, c1, c2);
}

// A tangent normal as the height derivative in UV, (dH/du, dH/dv): z kept positive and above
// 1/128 of the slope so the derivative stays within +-128 (reference TspaceNormalToDerivative;
// greenSign -1 is its gFlipVertDeriv, set for the same reason: D3D's V runs down the image too)
vec2 stochasticNormalToDerivative(vec3 n, float greenSign){
    vec3 a = abs(n);
    float z = max(a.z, (1.0 / 128.0) * max(a.x, a.y));
    return -vec2(n.x, greenSign * n.y) / z;
}

vec3 stochasticBlendNormals(StochasticTiling st, vec3 n0, vec3 n1, vec3 n2, float strength, float greenSign){
    // a tap read the texture turned by its cell's rotation: its slope turns back with it
    vec2 d = vec2(0.0);
    if (st.weights.x > 0.0) d += stochasticRotateBack(stochasticNormalToDerivative(n0, greenSign), st.rot0) * st.weights.x;
    if (st.weights.y > 0.0) d += stochasticRotateBack(stochasticNormalToDerivative(n1, greenSign), st.rot1) * st.weights.y;
    if (st.weights.z > 0.0) d += stochasticRotateBack(stochasticNormalToDerivative(n2, greenSign), st.rot2) * st.weights.z;
    vec3 n = normalize(vec3(-d.x, -greenSign * d.y, 1.0));
    n.xy *= strength;
    return n;
}

vec3 stochasticNormal(texture2D tex, sampler smp, StochasticTiling st, float strength, float greenSign){
    vec3 n0 = vec3(0.0, 0.0, 1.0);
    vec3 n1 = vec3(0.0, 0.0, 1.0);
    vec3 n2 = vec3(0.0, 0.0, 1.0);
    if (st.weights.x > 0.0) n0 = STOCHASTIC_DECODE_NORMAL(stochasticGrad(tex, smp, st, 0));
    if (st.weights.y > 0.0) n1 = STOCHASTIC_DECODE_NORMAL(stochasticGrad(tex, smp, st, 1));
    if (st.weights.z > 0.0) n2 = STOCHASTIC_DECODE_NORMAL(stochasticGrad(tex, smp, st, 2));
    return stochasticBlendNormals(st, n0, n1, n2, strength, greenSign);
}

vec3 stochasticNormalArray(texture2DArray tex, sampler smp, float layer, StochasticTiling st, float strength, float greenSign){
    vec3 n0 = vec3(0.0, 0.0, 1.0);
    vec3 n1 = vec3(0.0, 0.0, 1.0);
    vec3 n2 = vec3(0.0, 0.0, 1.0);
    if (st.weights.x > 0.0) n0 = stochasticGradArray(tex, smp, layer, st, 0).rgb * 2.0 - 1.0;
    if (st.weights.y > 0.0) n1 = stochasticGradArray(tex, smp, layer, st, 1).rgb * 2.0 - 1.0;
    if (st.weights.z > 0.0) n2 = stochasticGradArray(tex, smp, layer, st, 2).rgb * 2.0 - 1.0;
    return stochasticBlendNormals(st, n0, n1, n2, strength, greenSign);
}

#endif
