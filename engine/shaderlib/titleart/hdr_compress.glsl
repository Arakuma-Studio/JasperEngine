// The 3D title main pass's light encodings (docs/specs/S86-title-3d.md R30), ported from
// Pachislop src/engine/three/hdrResolve.ts by SafEight, used with the author's permission.
// - The tone-map-aware resolve: each MSAA sample of straight colour x (its brightest channel,
//   after exposure) is written through an invertible shoulder above the knee (Karis 2013),
//   hd_expand inverts it after the resolve.
// - The compressed-light fallback (no renderable, blendable or multisampled RGBA16F): SRGB8A8
//   holding s = a C / (1 + max(C)), decoded as C = (s / a) / (1 - max(s / a)). It keeps HDR
//   highlights (up to about 255x white) for the tone map and the glint's bright pass.
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
