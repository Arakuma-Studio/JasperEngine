#version 450
// @effect     strands
// @kind       paint
// @category   light
// @summary    Flowing ribbons of light along a path, coloured from a multi-stop palette
// @source     shaders.com Strands
// @cost       medium
// @time       speed, color-speed
// @suggest-for ribbons light-strands
// @param      colors palette "#00e5ff 0% #5b8cff 34% #b06bff 67% #ff5fa2 100%" -- Palette the strands are coloured from
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the palette
// @param      speed number 0.5 0 2 -- Speed of the waves
// @param      amplitude number 2 0 5 -- Height of the waves
// @param      frequency number 0.3 0 5 -- Number of waves along the path
// @param      line-count integer 8 1 16 -- Number of strands
// @param      line-width number 0.05 0 1 -- Width of a strand
// @param      softness number 0.05 0 1 -- Edge softness of a strand
// @param      spread number 0.2 0 1 -- How far apart the strands run
// @param      pin-edges boolean false -- Pinch the strands together and fade them at the path's ends
// @param      color-scale number 1 1 6 -- How many times the palette repeats along the path
// @param      color-variance number 1 0 1 -- How much the colour differs from strand to strand
// @param      color-speed number 1 0 2 -- Speed of the colours flowing along the path
// @param      start position "0% 50%" -- Start of the path
// @param      end position "100% 50%" -- End of the path

// Ported from shaders.com Strands (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (overRgb(ribbons, backToP3, tonePow): ribbonPathFrame, ribbonsField with
// gradientStopsInSpace, the back-conversion and toneLift). Two clocks: speed drives the waves,
// color-speed scrolls the colours.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    vec2 start;
    vec2 end;
    float colorsCount;
    float colorSpace;
    float speed;
    float amplitude;
    float frequency;
    float lineCount;
    float lineWidth;
    float softness;
    float spread;
    float pinEdges;
    float colorScale;
    float colorVariance;
    float colorSpeed;
} params;

FAM_DECLARE_PALETTE_IN_SPACE(colors)

vec4 jfx_paint(JfxContext c){
    float waveT = c.anim.x;
    float colorT = c.anim.y;

    // ribbonPathFrame: progress along the start -> end path, signed distance from it, and the
    // pinch and end fade of pinned strands
    vec2 startPos = vec2(params.start.x * c.aspect, params.start.y);
    vec2 endPos = vec2(params.end.x * c.aspect, params.end.y);
    vec2 aspectUV = vec2(c.uv.x * c.aspect, c.uv.y);
    vec2 direction = endPos - startPos;
    float len = max(length(direction), 1e-6);
    vec2 normalizedDir = direction / len;
    vec2 perpDir = vec2(-normalizedDir.y, normalizedDir.x);
    vec2 relativePos = aspectUV - startPos;
    float progress = dot(relativePos, normalizedDir) / len;
    float perpDist = dot(relativePos, perpDir);
    bool pinOn = params.pinEdges > 0.5;
    float pinch = pinOn ? sin(clamp(progress, 0.0, 1.0) * 3.141592653589793) : 1.0;
    float fade = smoothstep(0.0, 0.06, progress) * (1.0 - smoothstep(0.94, 1.0, progress));
    float edgeFade = pinOn ? fade : 1.0;

    float thickness = params.lineWidth * 0.07;
    float softness = params.softness * 0.08 + 0.0008;
    float denom = max(params.lineCount - 1.0, 1.0);
    float waveBase = progress * JFX_TAU * params.frequency;
    float shimmerBase = progress * 8.0 + waveT * 2.0;
    float colorBase = progress * params.colorScale + colorT;

    vec3 accumColor = vec3(0.0);
    float accumWeight = 0.0;
    int n = int(floor(params.lineCount + 0.5));
    for (int idx = 0; idx < 16; idx++){
        if (idx >= n)
            break;
        float fi = float(idx);
        float norm = fi / denom;
        float phase = fi * 0.9;
        float strandT = waveT * (norm * 0.6 + 0.7);
        // coherent flowing waves (two octaves)
        float wave = sin(waveBase + strandT + phase) + sin(waveBase * 2.1 - strandT * 0.7 + phase) * 0.5;
        float displacement = wave * params.amplitude * 0.06;
        float baseline = (norm - 0.5) * params.spread;
        float centre = (baseline + displacement) * pinch;
        float dd = abs(perpDist - centre);
        float line = 1.0 - smoothstep(thickness, thickness + softness, dd);
        float shimmer = sin(shimmerBase + fi) * 0.15 + 0.85;
        // the colour coordinate's seamless ping-pong, looked up in the working space
        float cc = colorBase + norm * params.colorVariance;
        float colorCoord = 1.0 - abs(fract(cc * 0.5) * 2.0 - 1.0);
        vec3 strandColor = fam_palette_in_space_colors(colorCoord, params.colorSpace).rgb;
        float weight = line * shimmer;
        accumColor += strandColor * weight;
        accumWeight += weight;
    }

    // the weighted mean colour, back to rgb, the pow 0.85 tone lift; coverage is the alpha
    vec3 working = accumColor / max(accumWeight, 1e-4);
    vec3 rgb = pow(max(jfx_from_space(working, params.colorSpace), vec3(0.0)), vec3(0.85));
    return vec4(rgb, clamp(accumWeight * edgeFade, 0.0, 1.0));
}

#include "effects/lib/main_paint.glsl"
