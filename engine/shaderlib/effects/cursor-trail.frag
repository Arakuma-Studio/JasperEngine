#version 450
// @effect     cursor-trail
// @kind       paint
// @category   interactive
// @summary    A fading paint stroke that follows the pointer
// @source     shaders.com CursorTrail (adapted: CPU trail buffer replaces the fluid grid)
// @cost       medium
// @pointer    trail
// @param      colors palette "#00aaff #ff00aa" -- Colours of the stroke from fresh to old
// @param      radius length 18px 2px 128px -- Radius of the stroke
// @param      lifetime time 0.5s 0.1s 2s -- How long the stroke lasts (source: length)
// @param      shrink number 1 0 1 -- How much the stroke tapers as it fades (0 none)
// @param      softness number 0 0 1 -- Edge softness (0 is a crisp paint edge)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the ramp

// Ported from shaders.com CursorTrail (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (agents.ts makeStrokePolylineScan: the midpoint quadratic-Bezier capsule chain, its
// depth-weighted age blend and late fade). The pointer history is the engine's trail buffer
// (32 samples, newest first, area-local CSS px, ages in seconds) instead of a data texture.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float radius;
    float lifetime;
    float shrink;
    float softness;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

uniform u_fs_effectTrail {
    vec4 points[32];    // xy local px, z age s, w valid; newest first
} trail;

vec4 jfx_paint(JfxContext c){
    const int SUBDIVISIONS = 3;
    float lifetime = max(params.lifetime, 1e-3);
    float baseRadius = max(params.radius, 0.5);
    float edgeW = 0.75 / max(c.dpr, 0.25) + params.softness * baseRadius;
    float sharp = 4.0 / baseRadius;
    float minSdf = 1e4;
    float ageSum = 0.0;
    float wSum = 0.0;
    vec3 prev = vec3(0.0);   // xy, age (0..1)
    vec3 prev2 = vec3(0.0);
    bool prevAlive = false;
    bool prev2Alive = false;
    for (int i = 0; i < 32; i++){
        vec4 point = trail.points[i];
        vec3 cur = vec3(point.xy, point.z / lifetime);
        bool alive = point.w > 0.5 && cur.z < 1.0;
        if (alive){
            if (prevAlive){
                // the quadratic around the joint prev, from mid(prev2, prev) to mid(prev, cur)
                vec3 s0 = prev2Alive ? (prev2 + prev) * 0.5 : prev;
                vec3 e = (prev + cur) * 0.5;
                vec3 s = s0;
                float jMin = 1e4;
                float jAge = 1.0;
                for (int k = 0; k < SUBDIVISIONS; k++){
                    float t = (float(k) + 1.0) / float(SUBDIVISIONS);
                    float omt = 1.0 - t;
                    vec3 b = omt * omt * s0 + 2.0 * omt * t * prev + t * t * e;
                    float ra = baseRadius * (1.0 - s.z * params.shrink);
                    float rb = baseRadius * (1.0 - b.z * params.shrink);
                    vec2 ba = b.xy - s.xy;
                    float tt = clamp(dot(c.px - s.xy, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
                    float dist = length(c.px - (s.xy + ba * tt)) - mix(ra, rb, tt);
                    if (dist < jMin){
                        jMin = dist;
                        jAge = mix(s.z, b.z, tt);
                    }
                    s = b;
                }
                minSdf = min(minSdf, jMin);
                float w = exp(-max(jMin, -baseRadius) * sharp);
                ageSum += jAge * w;
                wSum += w;
            }else{
                // the newest sample (or one whose newer neighbour is gone): a dot
                float dist = length(c.px - cur.xy) - baseRadius * (1.0 - cur.z * params.shrink);
                minSdf = min(minSdf, dist);
                float w = exp(-max(dist, -baseRadius) * sharp);
                ageSum += cur.z * w;
                wSum += w;
            }
        }
        prev2 = prev;
        prev2Alive = prevAlive;
        prev = cur;
        prevAlive = alive;
    }
    float age = ageSum / max(wSum, 1e-20);
    float coverage = (1.0 - smoothstep(-edgeW, edgeW, minSdf)) * (1.0 - smoothstep(0.8, 1.0, age));
    vec4 color = jfx_palette_colors(clamp(age, 0.0, 1.0), params.colorSpace);
    return vec4(color.rgb, color.a * coverage);
}

#include "effects/lib/main_paint.glsl"
