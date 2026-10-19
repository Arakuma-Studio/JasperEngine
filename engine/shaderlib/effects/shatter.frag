#version 450
// @effect     shatter
// @kind       filter
// @category   interactive
// @summary    Broken glass: the pointer shoves shards apart, cracks open and bend the content
// @source     shaders.com Shatter
// @cost       stateful
// @pointer    trail
// @state      shards rgba32f 16x1 nearest
// @step       shove shards
// @reseed     seed
// @suggest-for broken-glass cracked-glass
// @param      crack-width number 1 0.5 5 -- Thickness of the crack lines
// @param      intensity number 4 0 20 -- How much the shards shift
// @param      radius number 0.4 0.1 1 -- Reach of the pointer's shove (share of the area)
// @param      decay number 1 0.1 10 -- How fast shards return to rest (seconds)
// @param      seed number 2 0 50 -- Random seed of the shard pattern
// @param      chromatic-split number 1 0 5 -- RGB separation in the cracks
// @param      refraction-strength number 5 0 10 -- How much the cracks bend the content
// @param      shard-lighting number 0.1 0 0.5 -- Light on tilted shards
// @param      edges enum mirror stretch|transparent|mirror|wrap -- How content pushed beyond the edges is handled

// Ported from shaders.com Shatter (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/Shatter/index.ts, std/effects/fracture.ts crackGeom, crackRefractUV, crackShardCompose).
// Upstream integrates the 16 shards' springs on the CPU and precomputes the nearest/second-nearest
// Voronoi field on the GPU; here the springs are the 16 x 1 state `shards` (displacement, uv), a
// pass a step, and the draw finds the two nearest sites per fragment. The sites and their random
// directions are upstream's seededRandom (fract(sin(x) * 10000)) in single precision.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float crackWidth;
    float intensity;
    float radius;
    float decay;
    float seed;
    float chromaticSplit;
    float refractionStrength;
    float shardLighting;
    float edges;
} params;

JFX_STATE(shards)

const int SHARD_COUNT = 16;

float shatter_random(float x){
    float v = sin(x) * 10000.0;
    return v - floor(v);
}

vec2 shatter_site(int i){
    return vec2(shatter_random(params.seed + float(i) * 2.0), shatter_random(params.seed + float(i) * 2.0 + 1.0));
}

// the premultiplied content with the edge mode (applyEdgeHandlingExpr over the RTT)
vec4 shatter_tap(vec2 uv){
    return jfx_premultiply(jfx_src_edge(uv * frame.size.xy, params.edges));
}

vec4 jfx_filter(JfxContext c){
    // the nearest and second-nearest site (voronoiNearest2, in uv)
    float d1 = 99999.0, d2 = 99999.0;
    int i1 = 0, i2 = 0;
    for (int i = 0; i < SHARD_COUNT; i++){
        vec2 d = c.uv - shatter_site(i);
        float distSq = dot(d, d);
        if (distSq < d1){
            d2 = d1;
            i2 = i1;
            d1 = distSq;
            i1 = i;
        }else if (distSq < d2){
            d2 = distSq;
            i2 = i;
        }
    }
    vec2 nearestPos = shatter_site(i1);
    vec2 secondPos = shatter_site(i2);
    vec2 disp = shards_fetch(ivec2(i1, 0)).xy;
    // crackGeom
    vec2 fromNearest = c.uv - nearestPos;
    float minDist1 = length(fromNearest);
    float minDist2 = length(c.uv - secondPos);
    float edgeDiff = minDist2 - minDist1;
    float baseCrackIntensity = 1.0 - smoothstep(0.0, params.crackWidth * 0.005, edgeDiff);
    float crackIntensity = baseCrackIntensity * smoothstep(0.0, 0.01, length(disp));
    vec2 toNearest = fromNearest / max(minDist1, 1e-5);
    vec2 edgeNormal = vec2(toNearest.y * -1.0, toNearest.x);
    vec2 displacedUV = c.uv - disp;
    // crackRefractUV per channel (red 1, green 0, blue -1)
    vec2 refraction = edgeNormal * (params.refractionStrength * 0.01);
    float chroma = params.chromaticSplit * 0.005;
    vec4 normalColor = shatter_tap(displacedUV);
    vec4 rFinal = shatter_tap(displacedUV + (refraction + edgeNormal * chroma) * crackIntensity);
    vec4 gFinal = shatter_tap(displacedUV + refraction * crackIntensity);
    vec4 bFinal = shatter_tap(displacedUV + (refraction - edgeNormal * chroma) * crackIntensity);
    // crackShardCompose (premultiplied), then straight
    vec3 refractedRGB = vec3(rFinal.x, gFinal.y, bFinal.z);
    vec3 shadedRGB = normalColor.xyz * (1.0 - crackIntensity) + refractedRGB * crackIntensity;
    float displacementLength = length(disp);
    float tiltX = disp.x / (displacementLength + 0.001);
    float tiltY = disp.y / (displacementLength + 0.001);
    const vec2 LIGHT = vec2(0.4472136, 0.8944272);   // normalize(0.3, 0.6)
    float lightingFactor = 1.0 + (tiltX * LIGHT.x + tiltY * LIGHT.y) * params.shardLighting;
    float finalLighting = 1.0 + (lightingFactor - 1.0) * smoothstep(0.0, 0.02, displacementLength);
    return jfx_unpremultiply(vec4(shadedRGB * finalLighting, normalColor.w));
}

#ifdef JFX_STEP_FORM
vec4 jfx_step(JfxStep s){
    // shardPhysics, one shard a texel
    int i = s.cell.x;
    float dt = min(s.dt, 0.016);
    vec2 size = max(s.c.size, vec2(1.0));
    float aspect = size.x / max(1.0, size.y);
    vec2 pointer = s.pointer / size;
    vec2 vel = (s.pointerKnown && dt > 0.0) ? (s.pointer - s.pointerPrev) / size / dt : vec2(0.0);
    float speed = length(vel);
    float maxVelocity = 5.0 + params.intensity * 2.0;
    if (speed > maxVelocity){
        vel *= maxVelocity / speed;
        speed = maxVelocity;
    }
    vec2 cellPos = shatter_site(i);
    vec2 randomDir = vec2(shatter_random(params.seed + float(i) * 3.0), shatter_random(params.seed + float(i) * 3.0 + 1.0)) - 0.5;
    float dx = aspect >= 1.0 ? (cellPos.x - pointer.x) * aspect : cellPos.x - pointer.x;
    float dy = aspect >= 1.0 ? cellPos.y - pointer.y : (cellPos.y - pointer.y) / aspect;
    float dist = sqrt(dx * dx + dy * dy);
    vec2 current = shards_fetch(ivec2(i, 0)).xy * exp(-dt / max(0.01, params.decay));
    vec2 velocityD = vec2(0.0);
    if (s.pointerKnown && dist < params.radius && speed > 0.01){
        float influence = max(0.0, 1.0 - dist / params.radius);
        float influenceCurve = influence * influence;
        velocityD = vel * (influenceCurve * speed * params.intensity * dt * 0.5) + randomDir * (influenceCurve * speed * params.intensity * dt * 0.1);
    }
    // the injected splats shove the shards around them along their velocity (uv/s)
    for (int k = 0; k < 8; k++){
        if (k >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(k);
        vec2 d = (cellPos - sp.pos / size) * vec2(max(aspect, 1.0), max(1.0 / aspect, 1.0));
        float r = max(sp.radius / size.y, 1e-4);
        float influence = max(0.0, 1.0 - length(d) / r);
        velocityD += sp.vel / size * influence * influence * sp.strength * 0.5;
    }
    float lerpFactor = min(1.0, 1.0 * dt);
    return vec4(current + velocityD * lerpFactor, 0.0, 0.0);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
