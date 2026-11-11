// The 3D title main pass's light encodings (docs/specs/S86-title-3d.md R30), ported from
// Pachislop src/engine/three/hdrResolve.ts by SafEight, used with the author's permission.
// - The tone-map-aware resolve: each MSAA sample of straight colour x (its brightest channel,
//   after exposure) is written through an invertible shoulder above the knee (Karis 2013),
//   hd_expand inverts it after the resolve.
// - The compressed-light fallback (no renderable, blendable or multisampled RGBA16F): SRGB8A8
//   holding s = a C / (1 + max(C)), decoded as C = (s / a) / (1 - max(s / a)). It keeps HDR
//   highlights (up to about 255x white) for the tone map and the glint's bright pass. Like the
//   float path's shoulder at k = 1 it compresses highlights before the MSAA resolve averages them.
//   Supersampled (k > 1), where the float path's resolve averages light, the fallback holds
//   premultiplied light / HD_LIGHT instead (linear: its resolve averages light too); the main
//   pass dithers it by half a sRGB code, which the k x k texels of a pixel average out.
//
// - The AO prepass without float targets: the view normal octahedral in 8 bits a component and
//   the title-relative depth in 16 bits over +-HD_DEPTH radii, in RGBA8 (code 0: nothing drawn).
//
// Valid C++ over the shim of TitleToneMapTest (no GLSL-only syntax).

const float HD_KNEE = 0.76;
const float HD_H = 0.75;

// a sample's straight colour x exposure -> its shoulder-compressed colour
vec3 hd_shoulder(vec3 straightColor, float exposure) {
    float m = max(max(straightColor.x, straightColor.y), straightColor.z) * exposure;
    if (m <= HD_KNEE || exposure <= 0.0) return straightColor;
    float y = HD_KNEE + HD_H * (m - HD_KNEE) / (HD_H + m - HD_KNEE);
    return straightColor * (y / m);
}

// the inverse on a resolved straight colour
vec3 hd_expand(vec3 straightColor, float exposure) {
    float y = max(max(straightColor.x, straightColor.y), straightColor.z) * exposure;
    if (y <= HD_KNEE || exposure <= 0.0) return straightColor;
    float t = min((y - HD_KNEE) / HD_H, 1.0 - 1.0 / 4096.0);
    float x = HD_KNEE + HD_H * t / (1.0 - t);
    return straightColor * (x / y);
}

// premultiplied linear light (any brightness) -> the compressed premultiplied encoding
vec4 hd_encodeLight(vec4 light) {
    float a = clamp(light.w, 0.0, 1.0);
    if (a <= 0.0) return vec4(0.0);
    vec3 c = max(vec3(light.x, light.y, light.z) / a, vec3(0.0));
    vec3 s = c / (1.0 + max(max(c.x, c.y), c.z));
    return vec4(s.x * a, s.y * a, s.z * a, a);
}

// the encoding -> premultiplied linear light
vec4 hd_decodeLight(vec4 encoded) {
    float a = clamp(encoded.w, 0.0, 1.0);
    if (a <= 0.0) return vec4(0.0);
    vec3 s = max(vec3(encoded.x, encoded.y, encoded.z) / a, vec3(0.0));
    float m = min(max(max(s.x, s.y), s.z), 1.0 - 1.0 / 255.0);
    vec3 c = s / (1.0 - m);
    return vec4(c.x * a, c.y * a, c.z * a, a);
}

// the light the linear encoding holds at most, x white (3D highlights reach 60)
const float HD_LIGHT = 64.0;

// premultiplied linear light -> the linear encoding (k > 1)
vec4 hd_encodeLinear(vec4 light) {
    vec3 c = max(vec3(light.x, light.y, light.z), vec3(0.0)) / HD_LIGHT;
    return vec4(c.x, c.y, c.z, clamp(light.w, 0.0, 1.0));
}

// a main target texel -> premultiplied linear light; mode 0 plain light, 1 the compressed
// encoding, 2 the linear one
vec4 hd_decode(vec4 texel, float mode) {
    if (mode > 1.5) return vec4(texel.x * HD_LIGHT, texel.y * HD_LIGHT, texel.z * HD_LIGHT, clamp(texel.w, 0.0, 1.0));
    if (mode > 0.5) return hd_decodeLight(texel);
    return texel;
}

// the depth range the packed AO prepass holds, title radii each way
const float HD_DEPTH = 8.0;

float hd_signNotZero(float v) {
    return v >= 0.0 ? 1.0 : -1.0;
}

// (unit view normal, title-relative depth) -> RGBA8 codes (0..1)
vec4 hd_packNormalDepth(vec4 nd) {
    vec3 n = vec3(nd.x, nd.y, nd.z);
    n = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float ox = n.z >= 0.0 ? n.x : (1.0 - abs(n.y)) * hd_signNotZero(n.x);
    float oy = n.z >= 0.0 ? n.y : (1.0 - abs(n.x)) * hd_signNotZero(n.y);
    float t = clamp(nd.w / (2.0 * HD_DEPTH) + 0.5, 0.0, 1.0);
    float code = floor(t * 65534.0 + 0.5) + 1.0;
    float hi = floor(code / 256.0);
    return vec4(floor((ox * 0.5 + 0.5) * 255.0 + 0.5) / 255.0, floor((oy * 0.5 + 0.5) * 255.0 + 0.5) / 255.0, hi / 255.0,
                (code - hi * 256.0) / 255.0);
}

// RGBA8 codes -> (unit view normal, title-relative depth); a zero normal where nothing was drawn
vec4 hd_unpackNormalDepth(vec4 p) {
    float code = floor(p.z * 255.0 + 0.5) * 256.0 + floor(p.w * 255.0 + 0.5);
    if (code < 0.5) return vec4(0.0);
    float ox = p.x * 2.0 - 1.0, oy = p.y * 2.0 - 1.0;
    float nz = 1.0 - abs(ox) - abs(oy);
    float nx = nz >= 0.0 ? ox : (1.0 - abs(oy)) * hd_signNotZero(ox);
    float ny = nz >= 0.0 ? oy : (1.0 - abs(ox)) * hd_signNotZero(oy);
    float l = sqrt(nx * nx + ny * ny + nz * nz);
    return vec4(nx / l, ny / l, nz / l, ((code - 1.0) / 65534.0 - 0.5) * 2.0 * HD_DEPTH);
}
