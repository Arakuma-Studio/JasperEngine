#version 450
// @effect     starfield
// @kind       paint
// @category   space
// @summary    Twinkling stars drifting in parallax layers, or streaking toward the centre
// @source     original
// @cost       medium
// @time       speed
// @suggest-for stars space galaxy
// @param      color color #ffffff -- Colour of the stars
// @param      density number 1 0.1 4 -- How many stars
// @param      layers integer 3 1 4 -- Number of parallax layers
// @param      speed number 0.05 -1 1 -- Drift speed (area heights per second for the nearest layer)
// @param      direction angle 90deg 0deg 360deg -- Direction of the drift (0deg up, 90deg right)
// @param      twinkle number 0.5 0 1 -- How much the stars twinkle
// @param      size number 1 0.5 3 -- Size of the stars
// @param      warp number 0 0 1 -- Streaks the stars toward the centre (hyperspace)

// Original effect: per layer, one star per lattice cell at a hashed position (the include
// library's hashes, after the shaders.com kit, MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt) with a hashed presence, brightness and twinkle phase. Farther
// layers have smaller cells, dimmer and smaller stars and drift slower. warp stretches each star
// into a streak along the line through the centre.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_sdf.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float density;
    float layers;
    float speed;
    float direction;
    float twinkle;
    float size;
    float warp;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 dir = vec2(sin(params.direction), -cos(params.direction));
    vec2 aspectUV = vec2(c.uv.x * c.aspect, c.uv.y);
    vec2 centre = vec2(c.aspect * 0.5, 0.5);
    // one CSS px in area-height units
    float pixel = 1.0 / max(c.size.y, 1.0);
    int count = int(clamp(params.layers, 1.0, 4.0) + 0.5);
    float light = 0.0;
    for (int i = 0; i < 4; i++){
        if (i >= count)
            break;
        float fi = float(i);
        float depth = 1.0 / (1.0 + fi * 0.8);               // 1 nearest .. 0.29 farthest
        float cells = 9.0 / depth;                           // cells per area height
        vec2 layerOffset = vec2(fi * 17.31, fi * 41.77);
        // the drift, in cells, slower for farther layers
        vec2 shift = dir * c.anim.x * depth;
        // warp: the streak runs along the radial line, toward the centre
        vec2 radial = aspectUV - centre;
        float radialLen = length(radial);
        vec2 radialDir = radialLen > 1e-5 ? radial / radialLen : vec2(0.0, 1.0);
        vec2 p = (aspectUV - shift) * cells + layerOffset;
        vec2 cell = floor(p);
        // the 3x3 neighbourhood, so glows and streaks cross the cell edges
        for (int y = -1; y <= 1; y++){
            for (int x = -1; x <= 1; x++){
                vec2 id = cell + vec2(float(x), float(y));
                vec3 h = jfx_hash32(id + fi * 0.37);
                // presence: density 1 lights about a third of the cells
                if (h.z > min(params.density * 0.33, 0.95))
                    continue;
                vec2 starP = id + 0.15 + h.xy * 0.7;
                vec2 d = (p - starP) / cells;               // area-height units
                // the streak: d measured with the radial component shrunk
                float streak = params.warp * (0.02 + 0.25 * radialLen) * depth;
                float along = dot(d, radialDir);
                vec2 perp = d - along * radialDir;
                float alongSq = max(abs(along + streak * 0.5) - streak * 0.5, 0.0);
                float dist = length(vec2(alongSq, length(perp)));
                float brightness = 0.35 + 0.65 * fract(h.x * 7.31 + h.y * 3.17);
                float radius = pixel * params.size * (0.6 + 1.1 * depth) * (0.6 + 0.6 * brightness);
                float core = 1.0 - smoothstep(radius * 0.5, radius * 1.5, dist);
                float glow = exp(-dist / (radius * 2.5)) * 0.35;
                float phase = fract(h.y * 13.7 + h.x * 5.3) * JFX_TAU;
                float rate = 1.5 + 3.0 * fract(h.x * 31.1);
                float tw = mix(1.0, 0.5 + 0.5 * sin(c.time * rate + phase), params.twinkle);
                light += (core + glow) * brightness * tw * (0.45 + 0.55 * depth);
            }
        }
    }
    return vec4(params.color.rgb, params.color.a * clamp(light, 0.0, 1.0));
}

#include "effects/lib/main_paint.glsl"
