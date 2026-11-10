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
