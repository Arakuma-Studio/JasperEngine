// HTML UI effects, filter family (p5-fx-filters): the glass filters' optics. Ported from the
// shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): effects/glass.ts (glassRefrStrength, glassTint,
// glassBorderHighlight, glassSpecular, glassFresnelRim, glassComposite on a flat 2D field),
// warpMaps.ts (flutedGlassGeom, blinnHighlight) and motionBlur.ts (glassTilesUV), with their
// constants. Angles are radians.
//
// The glass shape is the element's rounded box (jfx_box_sdf, CSS px). Where the kit measures in
// shape units (its default shape is 0.7 across), the element's shorter side is 0.7 units.
#ifndef FAM_FILTERS_GLASS_GLSL
#define FAM_FILTERS_GLASS_GLSL

#include "effects/lib/jfx_common.glsl"

// CSS px per shape unit: the shorter side is 0.7 units
float fam_glass_unit(vec2 size){
    return max(min(size.x, size.y), 1e-3) / 0.7;
}

// glassRefrStrength of a flat 2D plate: (1 - depth)^2, the depth over `thickness` px
float fam_glass_refr_strength(float sdfPx, float thicknessPx){
    float depthNorm = clamp(-sdfPx / max(thicknessPx, 0.5), 0.0, 1.0);
    return (1.0 - depthNorm) * (1.0 - depthNorm);
}

// glassTint: mix toward the tint, optionally rescaled to the original Rec. 601 luminance
vec3 fam_glass_tint(vec3 rgb, vec3 tintColor, float tintIntensity, float preserveLum){
    vec3 lumWeights = vec3(0.299, 0.587, 0.114);
    float origLum = dot(rgb, lumWeights);
    vec3 tinted = mix(rgb, tintColor, tintIntensity);
    float tintedLum = dot(tinted, lumWeights);
    vec3 lumPreserved = tinted * (origLum / max(tintedLum, 0.0001));
    return mix(tinted, lumPreserved, preserveLum > 0.5 ? 1.0 : 0.0);
}

// glassBorderHighlight: a thin ring inside the edge (px), lit where the normal faces the light
float fam_glass_border_highlight(float sdfPx, float edgeSoftness, float unitPx, float pxH, vec2 grad, float lightAngle, float highlight){
    float sharp = max(edgeSoftness * 0.5, 0.001);
    float w2 = max(sharp * 0.0625 * unitPx, pxH * 1.5);
    float rb2base = clamp(-(sdfPx - pxH) / w2, 0.0, 1.0) - clamp(-sdfPx / w2, 0.0, 1.0);
    float lightFacing = clamp(dot(grad, vec2(cos(lightAngle), sin(lightAngle))) * 0.5 + 0.5, 0.0, 1.0);
    return rb2base * lightFacing * highlight;
}

// glassSpecular: a Phong glint on the field's normal (view +z)
float fam_glass_specular(vec2 grad, float lightAngle, float highlight, float highlightSoftness, float refrStrength){
    const float INV_HLEN = 0.4472135954999579;
    float nLen = sqrt(dot(grad, grad) + 4.0);
    vec3 n = vec3(grad, 2.0) / nLen;
    float nDotH = n.x * (cos(lightAngle) * INV_HLEN) + n.y * (sin(lightAngle) * INV_HLEN) + n.z * (2.0 * INV_HLEN);
    float shininess = exp2(8.0 - highlightSoftness * 7.0);
    return pow(clamp(nDotH, 0.0, 1.0), shininess) * highlight * refrStrength;
}

// glassFresnelRim of a flat plate: (1 - depth)^2 over fresnelSoftness x 0.06 units
float fam_glass_fresnel_rim(float sdfPx, float unitPx, float fresnel, float fresnelSoftness, float rb1){
    float fresnelWidth = max(fresnelSoftness * 0.06, 0.001) * unitPx;
    float fresnelDepth = clamp(-sdfPx / fresnelWidth, 0.0, 1.0);
    return (1.0 - fresnelDepth) * (1.0 - fresnelDepth) * fresnel * rb1;
}

// ---- fluted glass (warpMaps.ts)

struct FamFluteGeom {
    vec2 refractedUV;
    vec2 chrOff;
    float slope;
};

// flutedGlassGeom: shape 0 bars, 1 rounded, 2 waves; t the (negated) clock
FamFluteGeom fam_flute_geom(vec2 uv, float aspect, float t, float angle, float frequency, float softness, float waveAmp, float waveFreq,
                            float refraction, float aberration, float shape){
    float expHi = shape < 0.5 ? 16.0 : 8.0;
    float expLo = shape < 0.5 ? 4.0 : 3.0;
    float wavesFlag = shape > 1.5 ? 1.0 : 0.0;
    float cosA = cos(angle);
    float sinA = sin(angle);
    vec2 aspCorr = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);
    float u = aspCorr.x * cosA + aspCorr.y * sinA;
    float v = -aspCorr.x * sinA + aspCorr.y * cosA;
    float waveTerm = sin(v * waveFreq * JFX_TAU + t * 2.0) * waveAmp;
    float flutePos = (u + wavesFlag * waveTerm) * frequency + t;
    float cellPos = (fract(flutePos) - 0.5) * 2.0;
    float absCell = max(abs(cellPos), 0.0001);
    float slope = sign(cellPos) * pow(absCell, mix(expHi, expLo, softness));
    float halfCell = 0.5 / max(frequency, 0.001);
    float refrU = -(slope * refraction * halfCell);
    FamFluteGeom g;
    g.refractedUV = vec2(uv.x + refrU * cosA / aspect, uv.y + refrU * sinA);
    float chrU = refrU * aberration * 0.5;
    g.chrOff = vec2(chrU * cosA / aspect, chrU * sinA);
    g.slope = slope;
    return g;
}

// blinnHighlight: Blinn-Phong with Schlick-Fresnel over a 1D slope (light 0 head-on, pi/2 grazing)
float fam_blinn_highlight(float slope, float lightAngle, float intensity, float softness){
    float nz = sqrt(1.0 - min(slope * slope, 1.0));
    float halfAngle = lightAngle * 0.5;
    float nDotH = max(slope * sin(halfAngle) + nz * cos(halfAngle), 0.0);
    float shininess = exp2(8.0 - softness * 7.0);
    float fresnelMix = 0.04 + 0.96 * pow(1.0 - nz, 5.0);
    return pow(nDotH, shininess) * fresnelMix * intensity;
}

// ---- glass tiles (motionBlur.ts glassTilesUV)

vec2 fam_glass_tiles_uv(vec2 uv, float aspect, float intensity, float baseTileCount, float rotation, float roundness){
    bool isWide = aspect > 1.0;
    vec2 tileCount = vec2(isWide ? baseTileCount : baseTileCount * aspect, isWide ? baseTileCount / aspect : baseTileCount);
    float cosA = cos(rotation);
    float sinA = sin(rotation);
    vec2 centered = vec2(uv.x * aspect, uv.y) - vec2(0.5 * aspect, 0.5);
    vec2 rotatedUV = vec2(centered.x * cosA - centered.y * sinA, centered.x * sinA + centered.y * cosA) + vec2(0.5 * aspect, 0.5);
    vec2 gridUV = vec2(rotatedUV.x / aspect, rotatedUV.y);
    vec2 cellCoord = floor(gridUV * tileCount) / tileCount;
    vec2 normalizedLocalUV = (gridUV - cellCoord) / (vec2(1.0) / tileCount);
    vec2 fromCenter = normalizedLocalUV - vec2(0.5);
    float distFromCenter = fromCenter.x * fromCenter.x + fromCenter.y * fromCenter.y;
    float clampedRoundness = max(1.0 - distFromCenter * (roundness * 4.0), 0.0);
    float scaleFactor = intensity * 0.025 * clampedRoundness;
    return uv + vec2(fromCenter.x * scaleFactor / aspect, fromCenter.y * scaleFactor);
}

#endif // FAM_FILTERS_GLASS_GLSL
