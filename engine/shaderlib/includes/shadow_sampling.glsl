// 3D shadow lookups (spec S10): fragment-stage shadow coordinates, texel-unit depth bias and
// normal offset, the receiver-plane depth bias, the Hard / grid / Vogel / PCSS filters, the
// cascade blend band and distance fade, and the point lights' cube faces in their depth atlas.
// The blocks and textures are includes/shadow_data.glsl's; includes/lights.glsl wraps these in
// lightShadow. Every lookup in a loop uses explicit LOD 0 (SampleCmpLevelZero on HLSL), so no
// backend has to unroll a shadow loop, and the loops carry [[dont_unroll]] ([loop] on HLSL).
//
// Taken from OpenEngine (MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// NOTICE.md), with every depth comparison inverted for our GL-convention depth: the receiver
// plane (shadow_receiver_plane.glsl), the Vogel disk and its rotation key
// (shadow_filter.glsl GE_VogelDisk, shadow_sampling.glsl GE_PoissonRotation), the cascade blend
// fraction (shadow_cascade_blend.glsl) and the distance fade (shadow_distance_fade.glsl).

#ifndef SHADOW_SAMPLING_GLSL
#define SHADOW_SAMPLING_GLSL

#extension GL_EXT_control_flow_attributes : enable

#include "includes/shadow_data.glsl"
#include "includes/shadow_pcss_decisions.glsl"

#ifdef GL_EXT_control_flow_attributes
    #define SHADOW_LOOP [[dont_unroll]]
#else
    #define SHADOW_LOOP
#endif

const int SHADOW_VOGEL_TAPS = 16;
const float SHADOW_VOGEL_MAX_TEXELS = 8.0;
// the reference distance of a soft light's Vogel radius: its penumbra two metres behind a blocker
const float SHADOW_SOFT_REFERENCE_DISTANCE = 2.0;

// ---------------------------------------------------------------- atlas lookups

// a slot-local UV into atlas UV
vec2 shadowAtlasCoord(vec4 rect, vec2 uv){
    vec2 atlasUV = rect.xy + uv * rect.zw;
    #ifndef IS_GLSL
        // slots are placed with sokol's bottom-left viewport convention, but only GL stores
        // and samples render targets bottom-up
        atlasUV.y = 1.0 - atlasUV.y;
    #endif
    return atlasUV;
}

// one bilinear comparison: the lit fraction of the 2x2 texels around uv (slot-local) at depth
float shadowCompare(bool pointAtlas, vec4 rect, vec2 uv, float depth){
    vec3 coord = vec3(shadowAtlasCoord(rect, uv), depth);
    #ifdef SHADOW_DATA_POINT
        if (pointAtlas){
            return textureLod(sampler2DShadow(u_shadowPointAtlas, u_shadowAtlas_smp), coord, 0.0);
        }
    #endif
    return textureLod(sampler2DShadow(u_shadowAtlas, u_shadowAtlas_smp), coord, 0.0);
}

// the slot-local UV a tap is kept inside: half a texel in from the slot's edges, so the
// bilinear footprint never reads a neighbouring slot
vec2 shadowClampToSlot(vec2 uv, float resolution){
    float inset = 0.5 / resolution;
    return clamp(uv, vec2(inset), vec2(1.0 - inset));
}

// interleaved gradient noise of a continuous key (Jimenez 2014): the filter rotation and the
// cascade dither, keyed on shadow texels so they are locked to the world (no TAA to hide a
// pattern that swims with the screen)
float shadowNoise(vec2 key){
    return fract(52.9829189 * fract(dot(key, vec2(0.06711056, 0.00583715))));
}

vec2 shadowRotation(vec2 key){
    float angle = shadowNoise(key) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

// The noise key of a receiver in a slot: its position in shadow texels of the light's own frame,
// so the pattern stays on the world when a stable cascade moves by whole texels (the slot-local
// uv * resolution would shift with it). A directional cascade's matrix is an ortho box times a
// rotation: its translation column is the box's (snapped) centre. A spot slot never moves.
vec2 shadowNoiseKey(mat4 lightVP, vec4 clip, vec2 uv, float resolution){
    // orthographic: its last row is (0, 0, 0, 1) (a perspective one holds the light's forward)
    if (abs(lightVP[0][3]) + abs(lightVP[1][3]) + abs(lightVP[2][3]) < 1e-5 && abs(lightVP[3][3] - 1.0) < 1e-5){
        // w = 1: undo the box's centre
        return (clip.xy - lightVP[3].xy) * (0.5 * resolution);
    }
    return uv * resolution;
}

// the i-th of n points of a unit Vogel disk (golden-angle spiral), rotated by sc = (cos, sin)
vec2 shadowVogelDisk(int i, int n, vec2 sc){
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * 2.39996323;
    float ct = cos(theta);
    float st = sin(theta);
    return r * vec2(ct * sc.x - st * sc.y, ct * sc.y + st * sc.x);
}

// ---------------------------------------------------------------- receiver plane

// (dDepth/du, dDepth/dv) of the receiver from its shadow-coordinate derivatives dx, dy (uv,
// depth); zero when they are degenerate. A tap at an offset compares against this plane.
vec2 shadowReceiverPlaneGradient(vec3 dx, vec3 dy){
    float determinant = dx.x * dy.y - dx.y * dy.x;
    float footprint = length(dx.xy) * length(dy.xy);
    if (footprint <= 1e-20 || abs(determinant) <= footprint * 1e-4)
        return vec2(0.0);
    vec2 gradient = vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / determinant;
    if (any(isnan(gradient)) || any(isinf(gradient)))
        return vec2(0.0);
    return clamp(gradient, vec2(-32.0), vec2(32.0));
}

// the derivatives of a slot's (uv, depth) from the world-position derivatives
vec3 shadowCoordDerivative(mat4 lightVP, vec4 clip, vec3 ndc, vec3 worldDelta){
    vec4 q = lightVP * vec4(worldDelta, 0.0);
    vec3 d = (q.xyz - ndc * q.w) / clip.w;
    return d * 0.5;
}

// ---------------------------------------------------------------- filters

// The depth a tap at offsetUV compares against: the receiver plane carried to the tap, minus
// the bilinear comparison's reach of one texel along it (the sampler mixes texels up to a
// texel away)
float shadowTapDepth(float depth, vec2 offsetUV, vec2 gradient, float texel){
    return depth + dot(offsetUV, gradient) - dot(abs(gradient), vec2(texel));
}

// a (2r+1)^2 box of bilinear taps in (r+1)^2 fetches: per axis the texels weigh
// [1-f, 1, .., 1, f], and one fetch per texel pair placed by the weights gives the same sum
float shadowGridPcf(bool pointAtlas, vec4 rect, vec2 uv, float depth, vec2 gradient, float resolution, int radius){
    float texel = 1.0 / resolution;
    vec2 texelPos = uv * resolution - 0.5;
    vec2 base = floor(texelPos);
    vec2 f = texelPos - base;
    float sum = 0.0;
    SHADOW_LOOP for (int kx = 0; kx <= 2; ++kx){
        if (kx > radius) break;
        float wx0 = (kx == 0) ? 1.0 - f.x : 1.0;
        float wx1 = (kx == radius) ? f.x : 1.0;
        float wx = wx0 + wx1;
        float ox = float(-radius + 2 * kx) + wx1 / wx + 0.5;
        SHADOW_LOOP for (int ky = 0; ky <= 2; ++ky){
            if (ky > radius) break;
            float wy0 = (ky == 0) ? 1.0 - f.y : 1.0;
            float wy1 = (ky == radius) ? f.y : 1.0;
            float wy = wy0 + wy1;
            float oy = float(-radius + 2 * ky) + wy1 / wy + 0.5;
            vec2 tapUV = shadowClampToSlot((base + vec2(ox, oy)) * texel, resolution);
            sum += wx * wy * shadowCompare(pointAtlas, rect, tapUV, shadowTapDepth(depth, tapUV - uv, gradient, texel));
        }
    }
    float taps = float(2 * radius + 1);
    return sum / (taps * taps);
}

#ifdef SHADOW_SAMPLING_HQ
// taps of a Vogel disk of radiusTexels, rotated by noise of the receiver's world-locked key
// (shadowNoiseKey)
float shadowVogelPcf(bool pointAtlas, vec4 rect, vec2 uv, float depth, vec2 gradient, float resolution, float radiusTexels, int taps, vec2 noiseKey){
    float texel = 1.0 / resolution;
    vec2 sc = shadowRotation(noiseKey);
    float sum = 0.0;
    SHADOW_LOOP for (int i = 0; i < SHADOW_PCSS_MAX_TAPS; ++i){
        if (i >= taps) break;
        vec2 offset = shadowVogelDisk(i, taps, sc) * (radiusTexels * texel);
        vec2 tapUV = shadowClampToSlot(uv + offset, resolution);
        sum += shadowCompare(pointAtlas, rect, tapUV, shadowTapDepth(depth, tapUV - uv, gradient, texel));
    }
    return sum / float(taps);
}
#endif

// The filter of a slot: mode 0 hard, 1 grid of radius gridRadius, 2 Vogel of radiusTexels
// (without SHADOW_SAMPLING_HQ: the Medium tier's 5x5 grid)
float shadowFilter(bool pointAtlas, vec4 rect, vec2 uv, float depth, vec2 gradient, float resolution, int mode, int gridRadius, float radiusTexels, vec2 noiseKey){
    if (mode == 1){
        return shadowGridPcf(pointAtlas, rect, uv, depth, gradient, resolution, gridRadius);
    }
    if (mode >= 2){
        #ifdef SHADOW_SAMPLING_HQ
            return shadowVogelPcf(pointAtlas, rect, uv, depth, gradient, resolution, radiusTexels, SHADOW_VOGEL_TAPS, noiseKey);
        #else
            return shadowGridPcf(pointAtlas, rect, uv, depth, gradient, resolution, 2);
        #endif
    }
    float texel = 1.0 / resolution;
    return shadowCompare(pointAtlas, rect, shadowClampToSlot(uv, resolution), depth - dot(abs(gradient), vec2(texel)));
}

#if defined(SHADOW_DATA_RAW) && defined(SHADOW_SAMPLING_HQ)
// a raw depth of the projective atlas (slot-local uv)
float shadowRawDepth(vec4 rect, vec2 uv){
    return textureLod(sampler2D(u_shadowAtlasRaw, u_shadowAtlasRaw_smp), shadowAtlasCoord(rect, uv), 0.0).r;
}

// PCSS of a projective slot: the average blocker of a Vogel search, its physical penumbra,
// then a Vogel PCF that wide. spotNearFar.x > 0 marks a spot slot (perspective depth, linear
// distances from the light), else the slot is a directional cascade.
float shadowPcss(int slot, vec2 uv, float depth, vec2 gradient, float texelWorld, float lightSize, vec2 spotNearFar, vec2 noiseKey){
    vec4 rect = shadowData.atlasRect[slot];
    float resolution = shadowData.split[slot].w;
    float texel = 1.0 / resolution;
    bool spot = spotNearFar.x > 0.0;
    float n = spotNearFar.x;
    float f = spotNearFar.y;
    float span = shadowData.bias[slot].w;

    // linear distance from the light of a window depth (spot)
    #define SHADOW_SPOT_DISTANCE(d) (2.0 * n * f / (f + n - (2.0 * (d) - 1.0) * (f - n)))

    float receiverDistance = spot ? SHADOW_SPOT_DISTANCE(depth) : 0.0;
    float widest = spot ? shadowPcssSpotPenumbraWorld(receiverDistance, n, lightSize)
                        : shadowPcssDirectionalPenumbraWorld(0.0, depth, span, lightSize);
    float searchTexels = shadowPcssSearchTexels(widest / max(texelWorld, 0.000001));

    vec2 sc = shadowRotation(noiseKey);
    float blockerSum = 0.0;
    int blockers = 0;
    SHADOW_LOOP for (int i = 0; i < SHADOW_PCSS_SEARCH_TAPS; ++i){
        vec2 offset = shadowVogelDisk(i, SHADOW_PCSS_SEARCH_TAPS, sc) * (searchTexels * texel);
        vec2 tapUV = shadowClampToSlot(uv + offset, resolution);
        float tapDepth = shadowRawDepth(rect, tapUV);
        if (shadowPcssIsBlocker(tapDepth, depth, tapUV - uv, gradient)){
            blockerSum += tapDepth;
            blockers++;
        }
    }
    if (blockers == 0){
        return 1.0;
    }
    float avgBlocker = blockerSum / float(blockers);
    float penumbraWorld = spot ? shadowPcssSpotPenumbraWorld(receiverDistance, SHADOW_SPOT_DISTANCE(avgBlocker), lightSize)
                               : shadowPcssDirectionalPenumbraWorld(avgBlocker, depth, span, lightSize);
    float radiusTexels = shadowPcssPenumbraTexels(penumbraWorld, texelWorld);
    #undef SHADOW_SPOT_DISTANCE
    return shadowVogelPcf(false, rect, uv, depth, gradient, resolution, radiusTexels, shadowPcssTapsForRadius(radiusTexels), noiseKey);
}
#endif

// ---------------------------------------------------------------- projective slots

bool shadowReceiverPlaneEnabled(){
    return mod(shadowData.header.z, 2.0) >= 1.0;
}

// The lit fraction of a projective slot at a (biased) world position: 1 outside the slot.
// lightDistance: from a spot light (0 for a directional light).
float shadowSlotVisibility(int slot, vec3 worldPos, vec3 dpdx, vec3 dpdy, float texelWorld, float lightDistance){
    mat4 lightVP = shadowData.lightVP[slot];
    vec4 clip = lightVP * vec4(worldPos, 1.0);
    if (clip.w <= 0.0)
        return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float depth = ndc.z * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0)
        return 1.0;

    vec2 gradient = vec2(0.0);
    if (shadowReceiverPlaneEnabled()){
        vec3 dx = shadowCoordDerivative(lightVP, clip, ndc, dpdx);
        vec3 dy = shadowCoordDerivative(lightVP, clip, ndc, dpdy);
        gradient = shadowReceiverPlaneGradient(dx, dy);
    }

    vec4 filterParams = shadowData.filtering[slot];
    int mode = int(filterParams.x + 0.5);
    float resolution = shadowData.split[slot].w;
    float radiusTexels = filterParams.y;
    vec2 noiseKey = shadowNoiseKey(lightVP, clip, uv, resolution);
    if (mode >= 2 && radiusTexels <= 0.0){
        // a spot light's soft radius: its penumbra at the reference distance behind a blocker
        float tanHalf = filterParams.z / max(lightDistance, 0.001);
        radiusTexels = clamp(tanHalf * SHADOW_SOFT_REFERENCE_DISTANCE / max(texelWorld, 0.000001), 1.0, SHADOW_VOGEL_MAX_TEXELS);
    }
    #if defined(SHADOW_DATA_RAW) && defined(SHADOW_SAMPLING_HQ)
        if (mode == 3 && shadowData.header.z >= 8.0){
            vec2 spotNearFar = (lightDistance > 0.0) ? shadowData.split[slot].xy : vec2(0.0);
            return shadowPcss(slot, uv, depth, gradient, texelWorld, filterParams.z, spotNearFar, noiseKey);
        }
    #endif
    return shadowFilter(false, shadowData.atlasRect[slot], uv, depth, gradient, resolution, mode, int(radiusTexels + 0.5), radiusTexels, noiseKey);
}

// the receiver offset by a slot's biases: towards the light by the depth bias, along the normal
// by the normal offset (more the more it faces away from the light)
vec3 shadowBiasedPosition(vec3 worldPos, vec3 worldNormal, vec3 towardsLight, float depthBias, float normalOffset){
    float NdotL = clamp(dot(worldNormal, towardsLight), 0.0, 1.0);
    return worldPos + towardsLight * depthBias + worldNormal * (normalOffset * (1.0 - NdotL));
}

// view depth of a world position along the camera the cascades are fitted to
float shadowViewDepth(vec3 worldPos){
    return dot(shadowData.fitCameraForward.xyz, worldPos - shadowData.fitCameraPosition.xyz);
}

// the cascade of a view depth (-1 past the last)
int shadowCascadeOf(int firstSlot, int cascades, float viewDepth){
    SHADOW_LOOP for (int c = 0; c < MAX_SHADOWCASCADES; ++c){
        if (c >= cascades) break;
        if (viewDepth <= shadowData.split[firstSlot + c].y)
            return c;
    }
    return -1;
}

// The lit fraction of a directional light (1 = lit): its cascade, the blend into the next one
// over the band before the split (or a world-locked dither on the Low tier), the fade to lit
// over the end of its shadow distance, its strength
float shadowDirectional(int firstSlot, int cascades, vec3 worldPos, vec3 worldNormal, vec3 towardsLight, vec3 dpdx, vec3 dpdy){
    float viewDepth = shadowViewDepth(worldPos);
    int cascade = shadowCascadeOf(firstSlot, cascades, viewDepth);
    if (cascade < 0 || viewDepth < 0.0)
        return 1.0;
    vec4 fadeRange = shadowData.fade[firstSlot];
    float fade = smoothstep(fadeRange.x, fadeRange.y, viewDepth);
    if (fade >= 1.0)
        return 1.0;

    int slot = firstSlot + cascade;
    float strength = shadowData.filtering[slot].w;
    float blendStart = shadowData.split[slot].z;
    float blendEnd = shadowData.split[slot].y;
    float blend = 0.0;
    if (cascade + 1 < cascades && viewDepth > blendStart && blendEnd > blendStart){
        blend = smoothstep(blendStart, blendEnd, viewDepth);
    }
    // the transition: 2 dithered, 4 blended, neither a hard switch
    float flags = shadowData.header.z;
    bool dither = mod(floor(flags / 2.0), 2.0) >= 1.0;
    bool blended = mod(floor(flags / 4.0), 2.0) >= 1.0;

    // both cascades sample the same biased position, with the texel size between theirs
    int next = slot + ((cascade + 1 < cascades) ? 1 : 0);
    if (dither && blend > 0.0){
        vec4 clip = shadowData.lightVP[slot] * vec4(worldPos, 1.0);
        vec2 key = shadowNoiseKey(shadowData.lightVP[slot], clip, clip.xy * 0.5 + 0.5, shadowData.split[slot].w);
        if (shadowNoise(key) < blend){
            slot = next;
        }
        blend = 0.0;
    }else if (!blended){
        blend = 0.0;
    }
    vec4 biasA = shadowData.bias[slot];
    vec4 biasB = shadowData.bias[next];
    float texelWorld = mix(biasA.z, biasB.z, blend);
    float scale = texelWorld / max(biasA.z, 0.000001);
    vec3 position = shadowBiasedPosition(worldPos, worldNormal, towardsLight, biasA.x * scale, biasA.y * scale);

    float visibility = shadowSlotVisibility(slot, position, dpdx, dpdy, biasA.z, 0.0);
    if (blend > 0.0){
        float nextVisibility = shadowSlotVisibility(next, position, dpdx, dpdy, biasB.z, 0.0);
        visibility = mix(visibility, nextVisibility, blend);
    }
    visibility = mix(1.0, visibility, strength);
    return mix(visibility, 1.0, fade);
}

// The lit fraction of a spot light (1 = lit)
float shadowSpot(int slot, vec3 lightPosition, vec3 worldPos, vec3 worldNormal, vec3 towardsLight, vec3 dpdx, vec3 dpdy){
    vec4 bias = shadowData.bias[slot];
    float lightDistance = length(worldPos - lightPosition);
    vec3 position = shadowBiasedPosition(worldPos, worldNormal, towardsLight, bias.x * lightDistance, bias.y * lightDistance);
    float visibility = shadowSlotVisibility(slot, position, dpdx, dpdy, bias.z * lightDistance, max(lightDistance, 0.001));
    return mix(1.0, visibility, shadowData.filtering[slot].w);
}

// One comparison tap of the main directional light at a world position (1 lit, 0 shadowed),
// with its depth bias and distance fade but no normal offset or filter: for passes that sample
// shadows many times per pixel along rays (volumetric fog). 1 when the scene has no shadowed
// directional light.
float sampleDirectionalShadow1(vec3 worldPos){
    int firstSlot = int(shadowData.header.x + 0.5);
    if (shadowData.header.x < -0.5)
        return 1.0;
    int cascades = int(shadowData.header.y + 0.5);
    float viewDepth = shadowViewDepth(worldPos);
    int cascade = shadowCascadeOf(firstSlot, cascades, viewDepth);
    if (cascade < 0 || viewDepth < 0.0)
        return 1.0;
    int slot = firstSlot + cascade;
    vec4 clip = shadowData.lightVP[slot] * vec4(worldPos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float depth = ndc.z * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0)
        return 1.0;
    vec4 bias = shadowData.bias[slot];
    float depthBias = bias.x / max(bias.w, 0.000001);
    float resolution = shadowData.split[slot].w;
    float visibility = shadowCompare(false, shadowData.atlasRect[slot], shadowClampToSlot(uv, resolution), depth - depthBias);
    visibility = mix(1.0, visibility, shadowData.filtering[slot].w);
    vec4 fadeRange = shadowData.fade[firstSlot];
    return mix(visibility, 1.0, smoothstep(fadeRange.x, fadeRange.y, viewDepth));
}

// ---------------------------------------------------------------- point lights

#ifdef SHADOW_DATA_POINT
// the cube face of a direction from the light: +X, -X, +Y, -Y, +Z, -Z
int shadowCubeFace(vec3 v){
    vec3 a = abs(v);
    if (a.x > a.y && a.x > a.z)
        return v.x > 0.0 ? 0 : 1;
    if (a.y > a.z)
        return v.y > 0.0 ? 2 : 3;
    return v.z > 0.0 ? 4 : 5;
}

// a direction in the camera space of a face (x, y across it, z along its axis), as the faces
// are rendered (RenderSystem::computeLightCameras' look-at bases)
vec3 shadowCubeFaceSpace(vec3 v, int face){
    if (face == 0) return vec3(-v.z, -v.y, v.x);
    if (face == 1) return vec3(v.z, -v.y, -v.x);
    if (face == 2) return vec3(v.x, v.z, v.y);
    if (face == 3) return vec3(v.x, -v.z, -v.y);
    if (face == 4) return vec3(v.x, -v.y, v.z);
    return vec3(-v.x, -v.y, -v.z);
}

// the face tile's (uv, depth) of a direction from the light
vec3 shadowCubeFaceCoord(vec3 faceSpace, vec4 projection){
    float z = max(faceSpace.z, 0.000001);
    vec2 uv = faceSpace.xy / (z * projection.x) * 0.5 + 0.5;
    return vec3(uv, projection.y + projection.z / z);
}

// The lit fraction of a point light (1 = lit): its cube face, rendered with a guard band wide
// enough for the widest kernel, so no tap leaves the face and the faces meet without seams
float shadowPoint(int firstSlot, vec3 lightPosition, vec3 worldPos, vec3 worldNormal, vec3 towardsLight, vec3 dpdx, vec3 dpdy){
    int light = firstSlot / SHADOW_CUBE_FACES;
    vec4 projection = pointShadowData.projection[light];
    vec4 bias = pointShadowData.bias[light];
    vec4 filterParams = pointShadowData.filtering[light];

    float lightDistance = length(worldPos - lightPosition);
    vec3 position = shadowBiasedPosition(worldPos, worldNormal, towardsLight, bias.x * lightDistance, bias.y * lightDistance);
    vec3 v = position - lightPosition;
    int face = shadowCubeFace(v);
    vec3 faceSpace = shadowCubeFaceSpace(v, face);
    vec3 coord = shadowCubeFaceCoord(faceSpace, projection);
    if (coord.z > 1.0)
        return 1.0;

    vec2 gradient = vec2(0.0);
    if (shadowReceiverPlaneEnabled()){
        vec3 dx = shadowCubeFaceCoord(shadowCubeFaceSpace(v + dpdx, face), projection) - coord;
        vec3 dy = shadowCubeFaceCoord(shadowCubeFaceSpace(v + dpdy, face), projection) - coord;
        gradient = shadowReceiverPlaneGradient(dx, dy);
    }

    int mode = int(filterParams.x + 0.5);
    float resolution = bias.w;
    float radiusTexels = 1.0;
    if (mode >= 2){
        float texelWorld = projection.w * lightDistance;
        float tanHalf = filterParams.z / max(lightDistance, 0.001);
        radiusTexels = clamp(tanHalf * SHADOW_SOFT_REFERENCE_DISTANCE / max(texelWorld, 0.000001), 1.0, SHADOW_VOGEL_MAX_TEXELS);
    }
    float visibility = shadowFilter(true, pointShadowData.faceRect[firstSlot + face], coord.xy, coord.z, gradient, resolution,
                                    min(mode, 2), int(filterParams.y + 0.5), radiusTexels, coord.xy * resolution);
    return mix(1.0, visibility, filterParams.w);
}
#endif

#endif // SHADOW_SAMPLING_GLSL
