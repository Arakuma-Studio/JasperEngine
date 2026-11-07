// Engine Water Excluders and Water Wakes for library shaders (manifest `@require waterexclusion`
// and `@require waterwake`, mesh shaders, fragment stage; library:water). The engine writes their
// data into the shader's own u_fs_customParams (instance customParams): put
// WATER_INTERACTION_PARAMS (includes/water_interaction_params.glsl, included before the block)
// last in that block, then include this file after it. Without the engine's bits nothing is
// declared: USE_WATER_EXCLUSION ('Wex') and USE_WATER_WAKE ('Wwk') come only while the scene has a
// Water Excluder / a Water Wake component, so a scene without them draws the shader as if it never
// asked.
//
//   bool waterExcluded(vec3 worldPosition);   // inside an excluder: the caller discards
//   vec3 waterWake(vec2 worldXZ);             // x foam 0..1, yz the wake waves' slope along X and Z
//
// Discard after every implicit-derivative read (fwidth, texture()), as library:water does: the
// lanes a discard ends would spoil their neighbours' derivatives on some backends.
//
// Engine data (RenderSystem::applyWaterInteraction, per draw; engine-written members, never set by
// a material):
//   customParams.waterExclusion:        x the excluders this draw tests (0..8): the ones over the
//                                       draw's mesh in the camera's view, nearest first; y, z, w
//                                       their rows in u_waterExclusionData, three bytes to a float
//   u_waterExclusionData:               the frame's excluders, a row each (RGBA32F, 4 wide): world
//                                       to 0..1 across the volume in texels 0..2, texel 3 x 0 box,
//                                       1 sphere, 2 hull, y its hull map layer
//   customParams.waterWakeArea:         xy world XZ of the wake map's corner, z 1 / its world size,
//                                       w 1 while it holds a wake
//   customParams.waterWakeParams:       x slope per unit of crest over a texel, y 1 / texels
//                                       across, z this water's body tag / 255
//   u_waterExclusionMaps:               a hull per layer seen from above (r its bottom, g its top
//                                       over 0..1 of its height, b under 0.5 inside its walls)
//   u_waterWakeTexture:                 r foam, g wave crest, b the body tag; repeating (each world
//                                       texel at its position modulo the size)

#ifndef WATER_INTERACTION_GLSL
#define WATER_INTERACTION_GLSL

#include "includes/water_interaction_params.glsl"

#ifdef USE_WATER_EXCLUSION

uniform texture2DArray u_waterExclusionMaps;
uniform sampler u_waterExclusionMaps_smp;
// @image_sample_type u_waterExclusionData unfilterable_float
uniform texture2D u_waterExclusionData;
// @sampler_type u_waterExclusionData_smp nonfiltering
uniform sampler u_waterExclusionData_smp;

vec4 waterExcluderTexel(int row, int texel){
    return texelFetch(sampler2D(u_waterExclusionData, u_waterExclusionData_smp), ivec2(texel, row), 0);
}

bool waterExcluded(vec3 worldPosition){
    vec4 p = vec4(worldPosition, 1.0);
    for (int i = 0; i < WATER_EXCLUDERS_PER_DRAW; i++){
        if (float(i) >= customParams.waterExclusion.x) break;
        int group = i / 3;
        float packed = group == 0 ? customParams.waterExclusion.y : (group == 1 ? customParams.waterExclusion.z : customParams.waterExclusion.w);
        int row = (int(packed) >> (8 * (i - group * 3))) & 255;
        vec3 q = vec3(dot(waterExcluderTexel(row, 0), p), dot(waterExcluderTexel(row, 1), p), dot(waterExcluderTexel(row, 2), p));
        if (any(lessThan(q, vec3(0.0))) || any(greaterThan(q, vec3(1.0))))
            continue;

        vec4 shape = waterExcluderTexel(row, 3);
        if (shape.x < 0.5)
            return true;
        if (shape.x < 1.5){
            if (dot(q - 0.5, q - 0.5) <= 0.25)
                return true;
        }else{
            // one level: no derivatives in a loop
            vec4 hull = textureLod(sampler2DArray(u_waterExclusionMaps, u_waterExclusionMaps_smp), vec3(q.xz, shape.y), 0.0);
            if (q.y >= hull.r && q.y <= hull.g && hull.b <= 0.5)
                return true;
        }
    }
    return false;
}

#endif // USE_WATER_EXCLUSION

#ifdef USE_WATER_WAKE

uniform texture2D u_waterWakeTexture;
uniform sampler u_waterWake_smp;

vec3 waterWake(vec2 worldXZ){
    if (customParams.waterWakeArea.w < 0.5)
        return vec3(0.0);
    // faded over the outer part of the map, none past it
    vec2 local = (worldXZ - customParams.waterWakeArea.xy) * customParams.waterWakeArea.z;
    float inside = smoothstep(0.0, 0.08, min(min(local.x, 1.0 - local.x), min(local.y, 1.0 - local.y)));
    if (inside <= 0.0)
        return vec3(0.0);

    vec2 uv = worldXZ * customParams.waterWakeArea.z;
    vec4 here = textureLod(sampler2D(u_waterWakeTexture, u_waterWake_smp), uv, 0.0);
    // another water's wake (or one the filter blends at its border) is not this water's
    float mine = 1.0 - step(0.5 / 255.0, abs(here.b - customParams.waterWakeParams.z));
    float texel = customParams.waterWakeParams.y;
    float crestX = textureLod(sampler2D(u_waterWakeTexture, u_waterWake_smp), uv + vec2(texel, 0.0), 0.0).g;
    float crestZ = textureLod(sampler2D(u_waterWakeTexture, u_waterWake_smp), uv + vec2(0.0, texel), 0.0).g;
    vec2 slope = (vec2(crestX, crestZ) - vec2(here.g)) * (customParams.waterWakeParams.x * inside * mine);
    return vec3(here.r * inside * mine, slope);
}

#endif // USE_WATER_WAKE

#endif // WATER_INTERACTION_GLSL
