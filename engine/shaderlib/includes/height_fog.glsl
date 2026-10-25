// Height fog (Fog type Height; HAS_HEIGHT_FOG in the fogged mesh variants): the medium, its lighting
// and the fog of any part of a view ray, shared by the fogged shaders (includes/fog.glsl), the
// sky composite, the volumetric fog march and the clouds' aerial perspective, so they agree
// where they meet. engine/core/render/AtmosphereMath.{h,cpp} mirrors it one to one.
//
// The medium: extinction sigma (1 / attenuation distance) up to the base height, falling as
// exp(-(y - base) / H) above it (1/1000 of it at the maximum height), over the part of a view
// ray from the start distance to the max fog distance. Lit by the fog colour (its Color, or
// the sky's horizon in the Sky colour mode) plus the sun's light scattered by a
// Henyey-Greenstein lobe: L = fogColor + sunColor * inscattering * 4 pi HG(g, cos) * visibility
// (visibility the sun's shadow in the volumetric march, 1 elsewhere). A part [t0, t1] of a ray
// lets T = exp(-optical depth) through (capped by the max opacity) and adds S = L (1 - T).
//
// The closed-form optical depth follows SanielX/Height-Fog (MIT) as OpenEngine ported it
// (height_fog_term.glsl, MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors),
// rewritten as an antiderivative of the density over height so parts of a ray compose exactly.
//
// Filled per camera pass by RenderSystem (height_fog_t in RenderSystem.h). The fragment's
// position comes from gl_FragCoord and the camera's inverse view-projection, in perspective
// and orthographic cameras alike.

#ifndef HEIGHT_FOG_GLSL
#define HEIGHT_FOG_GLSL

uniform u_fs_heightFog {
    mat4 invViewProjection;           // camera clip space -> world (the camera's own matrices, no target flip)
    vec4 screenToNdc;                 // camera ndc.xy = gl_FragCoord.xy * xy + zw (y up)
    vec4 color_colorMode;             // rgb linear fog colour, w 0 Color / 1 Sky (-1: the scene fog is not a height fog)
    vec4 extinction_base_H_maxOpacity; // x sigma, y base height, z scale height H, w max opacity
    vec4 start_volumetric_maxDist_inscatter; // x start distance, y the distance the volumetric fog of
                                      // this pass marches (0: none), z max fog distance, w directional
                                      // inscattering
    vec4 sunDir_anisotropy;           // xyz towards the main light, w anisotropy
    vec4 sunColor_skyFillSin;         // rgb main light colour * intensity (linear), w sin(sky fill angle), 0 = no fill
    vec4 horizonRing[4];              // rgb the sky's horizon at azimuths +Z, +X, -Z, -X (Sky colour mode);
                                      // [0].w the most the sky fill takes (0 = none)
} heightFog;

const float HEIGHT_FOG_LN_1000 = 6.907755279;

// world position of the fragment at gl_FragCoord.xy with window depth z01 (gl_FragCoord.z)
vec3 heightFogWorldPosition(vec2 fragCoord, float z01){
    vec2 ndc = fragCoord * heightFog.screenToNdc.xy + heightFog.screenToNdc.zw;
    vec4 world = heightFog.invViewProjection * vec4(ndc, z01 * 2.0 - 1.0, 1.0);
    return world.xyz / world.w;
}

// The view ray of the pixel at fragCoord: from the eye (perspective; the clip-space point
// (0, 0, 1, 0) of any projection) or from the near plane (orthographic: the eye is at infinity)
vec3 heightFogRayOrigin(vec2 fragCoord){
    vec4 eye = heightFog.invViewProjection[2];
    if (abs(eye.w) > 1e-7)
        return eye.xyz / eye.w;
    return heightFogWorldPosition(fragCoord, 0.0);
}

// The world position of the surface at fragCoord whose fog distance (gl_FragCoord.z /
// gl_FragCoord.w as it was drawn, includes/output.glsl) is fogDistance: z01 * w is linear in
// 1 / w along the pixel's ray, solved for z01
vec3 heightFogPositionAtFogDistance(vec2 fragCoord, float fogDistance){
    vec2 ndc = fragCoord * heightFog.screenToNdc.xy + heightFog.screenToNdc.zw;
    vec4 a = heightFog.invViewProjection * vec4(ndc, 0.0, 1.0);
    vec4 b = heightFog.invViewProjection[2];
    // fogDistance = (z + 1) / 2 / (a.w + z b.w), z the ndc depth
    float zNdc = (1.0 - 2.0 * fogDistance * a.w) / (2.0 * fogDistance * b.w - 1.0);
    vec4 world = a + zNdc * b;
    return world.xyz / world.w;
}

// relative density at a height
float heightFogDensity(float height){
    float above = height - heightFog.extinction_base_H_maxOpacity.y;
    return above <= 0.0 ? 1.0 : exp(-above / heightFog.extinction_base_H_maxOpacity.z);
}

// the integral of the density over height from the base height (negative below it)
float heightFogDensityIntegral(float height){
    float H = heightFog.extinction_base_H_maxOpacity.z;
    float above = height - heightFog.extinction_base_H_maxOpacity.y;
    return above <= 0.0 ? above : H * (1.0 - exp(-above / H));
}

// optical depth of origin + t dir (dir a unit vector) for t in [t0, t1], unclipped
float heightFogOpticalDepthUnclipped(vec3 origin, vec3 dir, float t0, float t1){
    float len = t1 - t0;
    if (len <= 0.0)
        return 0.0;
    float y0 = origin.y + t0 * dir.y;
    float y1 = origin.y + t1 * dir.y;
    float rise = y1 - y0;
    float sigma = heightFog.extinction_base_H_maxOpacity.x;
    if (abs(rise) < 1e-3 * heightFog.extinction_base_H_maxOpacity.z)
        return sigma * len * heightFogDensity(0.5 * (y0 + y1));
    return sigma * len * (heightFogDensityIntegral(y1) - heightFogDensityIntegral(y0)) / rise;
}

// the same over the part of [t0, t1] between the start and max fog distances
float heightFogOpticalDepth(vec3 origin, vec3 dir, float t0, float t1){
    float a = max(t0, heightFog.start_volumetric_maxDist_inscatter.x);
    float b = min(t1, heightFog.start_volumetric_maxDist_inscatter.z);
    return heightFogOpticalDepthUnclipped(origin, dir, a, b);
}

// Henyey-Greenstein times 4 pi (1 when isotropic); cosTheta between the view direction and
// the direction towards the light
float heightFogPhase(float anisotropy, float cosTheta){
    float g = clamp(anisotropy, -0.95, 0.95);
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-5);
    return (1.0 - g2) / (d * sqrt(d));
}

// the fog colour along dir: the Color, or the horizon ring blended by azimuth (Sky mode)
vec3 heightFogColor(vec3 dir){
    if (heightFog.color_colorMode.w < 0.5)
        return heightFog.color_colorMode.rgb;
    float planar = length(dir.xz);
    if (planar < 1e-5)
        return 0.25 * (heightFog.horizonRing[0].rgb + heightFog.horizonRing[1].rgb + heightFog.horizonRing[2].rgb + heightFog.horizonRing[3].rgb);
    vec2 h = dir.xz / planar;
    vec4 w = max(vec4(h.y, h.x, -h.y, -h.x), vec4(0.0));
    vec3 c = heightFog.horizonRing[0].rgb * w.x + heightFog.horizonRing[1].rgb * w.y
           + heightFog.horizonRing[2].rgb * w.z + heightFog.horizonRing[3].rgb * w.w;
    return c / (w.x + w.y + w.z + w.w);
}

// the sun's share of the light scattered towards the camera along dir, per unit of visibility
vec3 heightFogSunInscatter(vec3 dir){
    float phase = heightFogPhase(heightFog.sunDir_anisotropy.w, dot(dir, heightFog.sunDir_anisotropy.xyz));
    return heightFog.sunColor_skyFillSin.rgb * (heightFog.start_volumetric_maxDist_inscatter.w * phase);
}

// L along dir with the sun's visibility
vec3 heightFogInscatter(vec3 dir, float visibility){
    return heightFogColor(dir) + heightFogSunInscatter(dir) * visibility;
}

// the fog of the part [t0, t1] of a ray: what it adds and lets through
void heightFogSegment(vec3 origin, vec3 dir, float t0, float t1, out vec3 scattering, out float transmittance){
    float tau = heightFogOpticalDepth(origin, dir, t0, t1);
    transmittance = max(exp(-tau), 1.0 - heightFog.extinction_base_H_maxOpacity.w);
    scattering = heightFogInscatter(dir, 1.0) * (1.0 - transmittance);
}

// the fog over a surface at worldPosition seen through this pixel (the whole ray: the
// volumetric fog's composite replaces its near part, volumetric_composite.frag)
vec3 heightFogApply(vec3 color, vec2 fragCoord, vec3 worldPosition){
    vec3 origin = heightFogRayOrigin(fragCoord);
    vec3 toSurface = worldPosition - origin;
    float d = length(toSurface);
    if (d <= 1e-6)
        return color;
    vec3 dir = toSurface / d;
    vec3 scattering;
    float transmittance;
    heightFogSegment(origin, dir, 0.0, d, scattering, transmittance);
    return color * transmittance + scattering;
}

// how much of the fog colour a sky pixel in direction dir takes: all of it below the
// horizon, none above the sky fill angle, at most horizonRing[0].w
float heightFogSkyAmount(vec3 dir){
    float fillSin = heightFog.sunColor_skyFillSin.w;
    float maxFill = heightFog.horizonRing[0].w;
    if (fillSin <= 0.0 || maxFill <= 0.0)
        return 0.0;
    return maxFill * (1.0 - smoothstep(0.0, fillSin, dir.y));
}

#endif
