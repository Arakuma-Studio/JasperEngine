#version 450
// @effect     light-edge
// @kind       paint
// @category   light
// @summary    Glowing, pulsing spots of light racing around the element's rounded edge
// @source     shaders.com LightEdge (adapted: shape = element rounded box, sizes in px)
// @cost       medium
// @time       speed
// @geometry   bounds extent
// @suggest-for border-glow glow-border edge-light
// @param      colors palette "#7b2ff7 #00e0ff" -- Colours of the light spots, one orbit of spots per colour
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space overlapping lights blend in
// @param      thickness length 6px 1px 64px -- Width of the lit band along the edge
// @param      softness number 0.5 0 1 -- Edge softness (0 a crisp band, 1 a wide smooth glow)
// @param      intensity number 0.6 0 1 -- Brightness of the light spots
// @param      bloom number 0.3 0 1 -- How additively overlapping lights pile up (high values overdrive to white)
// @param      spots integer 3 1 5 -- Number of light spots orbiting per colour
// @param      spot-size number 0.5 0 1 -- Length of a light spot along the edge
// @param      pulse number 0 0 1 -- Heartbeat pulsing: syncs the lights to a double beat
// @param      smoke number 0.3 0 1 -- Luminous smoke drifting along the edge
// @param      smoke-size number 0.5 0 1 -- Scale of the smoke billows
// @param      speed number 1 0 4 -- Animation speed (0 pauses)
// @param      seed number 0 0 100 -- Re-rolls the spots' speeds, directions and phases
// @param      extent length 24px 0px 128px -- How far the light may reach outside the element

// Ported from shaders.com LightEdge (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/edgeGlow.ts: heartbeatPulse, edgeGlowBand, orbitSpotsAccum, the accumulation
// per colour, edgeGlowCompose), itself after paper-design/shaders "pulsing-border" (MIT).
// Adapted: the shape is the element's rounded box (jfx_box_sdf, CSS px) and the band's
// thickness is in px. The source's shape units (canvas heights, its default shape 0.7 across)
// map to the element's shorter side over 0.7; the smoke band is kept within the extent the
// geometry is drawn over, and the light fades out over the extent's last pixels so it is never
// cut by the geometry's edge. The orbit angle is about the element's centre.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/jfx_sdf.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float colorSpace;
    float thickness;
    float softness;
    float intensity;
    float bloom;
    float spots;
    float spotSize;
    float pulse;
    float smoke;
    float smokeSize;
    float speed;
    float seed;
    float extent;
} params;

// heartbeatPulse: two staggered sine spikes, period 1 / 0.18 clock seconds
float light_edge_heartbeat(float animTime){
    float time = 0.18 * animTime;
    float first = pow(abs(sin(time * JFX_TAU)), 10.0);
    float second = pow(abs(sin((time - 0.15) * JFX_TAU)), 10.0);
    return clamp(first + 0.6 * second, 0.0, 1.0);
}

// orbitSpotsAccum: up to 5 spots of one colour orbiting the edge; (over coverage, additive sum)
vec2 light_edge_orbit_spots(float angle01, float border, float animTime, float beat, float colorIdxF){
    float t = animTime * 1.2 + 131.0;
    float intensityEff = 1.0 + (1.0 + 4.0 * params.softness) * params.intensity;
    float spotsF = floor(params.spots + 0.5);
    float overAcc = 0.0;
    float addAcc = 0.0;
    for (int s = 0; s < 5; s++){
        float sF = float(s);
        if (sF >= spotsF)
            break;
        vec2 randVal = jfx_hash22(vec2(sF * 10.0 + 2.0 + params.seed * 7.31, 40.0 + colorIdxF + params.seed * 3.17));
        float speedFactor = 0.1 + 0.15 * abs(sin(sF * (2.0 + colorIdxF)) * cos(sF * (2.0 + 2.5 * colorIdxF)));
        float dir = randVal.y < 0.5 ? -1.0 : 1.0;
        float sTime = (speedFactor * t + randVal.x * 3.0) * dir;
        bool evenCond = abs(colorIdxF - 2.0 * floor(colorIdxF * 0.5)) < 0.5;
        float sinVal = sin(t + sF * (5.0 - 1.5 * colorIdxF));
        float cosVal = cos(t + sF * (3.0 + 1.3 * colorIdxF));
        float mask = 0.5 + 0.5 * (evenCond ? sinVal : cosVal);
        float pWeight = clamp(2.0 * params.pulse - randVal.x, 0.0, 1.0);
        mask = mix(mask, beat, pWeight);
        float atg = fract(angle01 + sTime);
        float ss = 0.05 + 0.6 * params.spotSize * params.spotSize + 0.05 * randVal.x;
        ss = mix(ss, 0.1, pWeight);
        float sector = smoothstep(0.5 - ss, 0.5, atg) * (1.0 - smoothstep(0.5, 0.5 + ss, atg));
        sector = clamp(sector * mask * border * intensityEff, 0.0, 1.0);
        overAcc = overAcc + (1.0 - overAcc) * sector;
        addAcc = addAcc + sector;
    }
    return vec2(overAcc, addAcc);
}

vec4 jfx_paint(JfxContext c){
    float animTime = c.anim.x;
    float beat = light_edge_heartbeat(animTime);
    float minSide = max(min(c.size.x, c.size.y), 1.0);
    float unitPx = minSide / 0.7;                       // px of one source shape unit
    float aa = 2.0 / max(c.dpr, 1e-3);                  // two device pixels
    float sdf = jfx_box_sdf(c.px, c.size, c.radii);

    // edgeGlowBand: the band hugging the edge, inset as it softens
    float th = max(0.5 * params.thickness, 0.5);
    float f = sdf + mix(th, 0.0, params.softness);
    float bd = abs(f);
    float borderTh = mix(th, 3.0 * th, params.softness);
    float e0 = min(mix(borderTh, -borderTh, params.softness), borderTh + aa);
    float e1 = max(mix(borderTh, -borderTh, params.softness), borderTh + aa);
    float border = 1.0 - smoothstep(e0, e1, bd);
    border = pow(clamp(border, 0.0, 1.0), 1.0 + params.softness);
    // the smoke: two counter-drifting value noises banded around the edge
    float t = animTime * 1.2 + 131.0;
    vec2 q = (c.px - c.size * 0.5) / unitPx;
    vec2 suv = q * (0.5 + 2.5 * params.smokeSize);
    float drift = 0.5 * t;
    float n1 = jfx_value12(vec2(suv.x * 2.7 + drift, suv.y * 2.7 + drift));
    float n2 = jfx_value12(vec2(suv.x * 3.4 - drift, suv.y * 3.4 - drift));
    float smoke = clamp(3.0 * n1, 0.0, 1.0) - n2;
    float smokeTh = min(clamp(th + 0.2 * unitPx, 0.1 * unitPx, 0.4 * unitPx), th + params.extent);
    float smokeBand = pow(clamp(1.0 - smoothstep(-smokeTh, smokeTh + aa, bd), 0.0, 1.0), 2.0);
    smoke = smoke * smokeBand;
    smoke = 30.0 * smoke * smoke * (0.5 * params.smoke * params.smoke);
    smoke = smoke * mix(1.0, beat, params.pulse);
    border = clamp(border + clamp(smoke, 0.0, 1.0), 0.0, 1.0);
    float angle01 = atan(q.y, q.x) / JFX_TAU;

    // one orbit of spots per palette colour, accumulated (edgeGlowAccumColor) and composed
    int n = int(clamp(params.colorsCount, 1.0, 8.0) + 0.5);
    vec3 rgb = vec3(0.0);
    float w = 0.0;
    float ba = 0.0;
    vec3 ac = vec3(0.0);
    float aAdd = 0.0;
    bool linearSpace = params.colorSpace < 0.5;
    for (int i = 0; i < 8; i++){
        if (i >= n)
            break;
        vec4 color = params.colors[i];
        vec2 sectors = light_edge_orbit_spots(angle01, border, animTime, beat, float(i));
        float wNew = color.a * sectors.x;
        float k = wNew / max(w + wNew, 0.00001);
        // the overlap blend in the colour space (both opaque: linear is a plain mix)
        rgb = linearSpace ? mix(rgb, color.rgb, k) : jfx_mix_colors(vec4(rgb, 1.0), vec4(color.rgb, 1.0), k, params.colorSpace).rgb;
        ba = ba + (1.0 - ba) * wNew;
        ac = ac + color.rgb * (color.a * sectors.y);
        aAdd = aAdd + color.a * sectors.y;
        w = w + wNew;
    }
    // edgeGlowCompose: the blend path mixed toward the additive pile-up by 4 x bloom
    float kb = params.bloom * 4.0;
    vec3 accumC = mix(rgb * ba, ac, vec3(kb));
    float accumA = clamp(mix(ba, aAdd, kb), 0.0, 1.0);
    vec3 straight = clamp(accumC / max(accumA, 0.0001), 0.0, 1.0);
    // never cut by the geometry: the light fades out over the extent's last pixels
    float fadeW = min(params.extent * 0.5, 4.0);
    float keep = 1.0 - fam_light_smoothstep(params.extent - fadeW, params.extent, sdf);
    return vec4(straight, accumA * keep);
}

#include "effects/lib/main_paint.glsl"
