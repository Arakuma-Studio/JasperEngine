// PCSS decisions as pure functions of their inputs (no resources, no derivatives), shared by the
// shadow sampler (includes/shadow_sampling.glsl) and its unit tests: this file is also valid
// C++ (engine/tests/core/render/PcssDecisionTest.cpp includes it with vec2, dot, abs, clamp and
// max defined), so keep to that subset.
//
// Depths are window depths of our GL convention: [0, 1], nearer the light is SMALLER (the
// comparison sampler's LESS_EQUAL: lit when the receiver is at or in front of the stored
// depth). Offsets are in shadow-slot UV; `gradient` is the receiver plane's depth change per
// UV (zero when the receiver-plane bias is off).
//
// Adapted from OpenEngine's shadow_pcss_decisions.glsl (reverse-Z there, so every comparison
// is inverted here): Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License (NOTICE.md).

#ifndef SHADOW_PCSS_DECISIONS_GLSL
#define SHADOW_PCSS_DECISIONS_GLSL

#ifdef __cplusplus
    #define SHADOW_PCSS_FN inline
#else
    #define SHADOW_PCSS_FN
#endif

// the widest penumbra in texels (the kernel cap of the blocker search and of the PCF)
const float SHADOW_PCSS_MAX_TEXELS = 24.0;
// the narrowest PCF kernel radius: a penumbra one texel wide at contact (the bilinear comparison
// adds its own texel), so a contact shadow is as sharp as the map allows
const float SHADOW_PCSS_MIN_TEXELS = 0.5;
// the blocker search's taps
const int SHADOW_PCSS_SEARCH_TAPS = 16;
// the PCF's taps: this many for a kernel up to SHADOW_PCSS_TAP_REF_TEXELS wide, then growing
// linearly with the width up to SHADOW_PCSS_MAX_TAPS
const int SHADOW_PCSS_MIN_TAPS = 16;
const int SHADOW_PCSS_MAX_TAPS = 32;
const float SHADOW_PCSS_TAP_REF_TEXELS = 16.0;

// Whether a raw depth at a tap `offsetUV` from the receiver centre occludes it: the reference
// is the receiver plane at the tap, the one the PCF compares against, so a sloped receiver's
// own surface inside the search disk is not its own blocker
SHADOW_PCSS_FN bool shadowPcssIsBlocker(float tapDepth, float refDepth, vec2 offsetUV, vec2 gradient){
    return tapDepth < refDepth + dot(offsetUV, gradient);
}

// The blocker search radius in texels: the widest penumbra the receiver could have (a blocker
// at the near plane), at least one texel, at most the kernel cap
SHADOW_PCSS_FN float shadowPcssSearchTexels(float widestPenumbraTexels){
    return clamp(widestPenumbraTexels, 1.0, SHADOW_PCSS_MAX_TEXELS);
}

// A directional light's penumbra in world units: the receiver-to-blocker depth (window depth
// times the ortho depth span) times tan(half the light's angular diameter)
SHADOW_PCSS_FN float shadowPcssDirectionalPenumbraWorld(float avgBlocker, float refDepth, float depthSpanWorld, float tanHalfAngle){
    return max(0.0, refDepth - avgBlocker) * depthSpanWorld * tanHalfAngle;
}

// A spot light's penumbra in world units at the receiver: similar triangles of a light of
// `lightRadius` (linear distances from the light along its axis)
SHADOW_PCSS_FN float shadowPcssSpotPenumbraWorld(float receiverDistance, float blockerDistance, float lightRadius){
    return lightRadius * max(0.0, receiverDistance - blockerDistance) / max(blockerDistance, 0.0001);
}

// The PCF kernel radius in texels of a penumbra: clamped to [half a texel, the cap]
SHADOW_PCSS_FN float shadowPcssPenumbraTexels(float penumbraWorld, float texelWorld){
    return clamp(penumbraWorld / max(texelWorld, 0.000001), SHADOW_PCSS_MIN_TEXELS, SHADOW_PCSS_MAX_TEXELS);
}

// The PCF taps for a kernel radius: the minimum up to the reference width, linear past it
SHADOW_PCSS_FN int shadowPcssTapsForRadius(float radiusTexels){
    float scaled = float(SHADOW_PCSS_MIN_TAPS) * radiusTexels / SHADOW_PCSS_TAP_REF_TEXELS;
    return clamp(int(scaled + 0.5), SHADOW_PCSS_MIN_TAPS, SHADOW_PCSS_MAX_TAPS);
}

#endif // SHADOW_PCSS_DECISIONS_GLSL
