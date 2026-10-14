#version 450
// @effect     aurora
// @kind       paint
// @category   gradient
// @summary    Aurora borealis: layered curtains with vertical rays and flowing light
// @source     shaders.com Aurora
// @cost       medium
// @time       speed
// @suggest-for northern-lights borealis
// @param      color-a color #a533f8 -- Edge colour at the curtain base
// @param      color-b color #22ee88 -- Core colour in the bright centre
// @param      color-c color #1694e8 -- Tip colour at the ray ends
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the colour mix
// @param      balance number 50 0 100 -- Shifts the colour distribution across the curtain height
// @param      intensity number 80 0 100 -- Overall brightness
// @param      curtains integer 4 1 4 -- Number of curtain layers
// @param      speed number 5 -10 10 -- Animation speed
// @param      waviness number 50 0 200 -- How much the curtains undulate
// @param      ray-density number 20 0 100 -- Density of the vertical rays
// @param      height number 120 10 200 -- How tall the aurora extends
// @param      center position "50% 0%" -- Where the curtains hang from
// @param      seed number 0 0 100 -- Random seed for variation

// Ported from shaders.com Aurora (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (Aurora/index.ts paint: wavyLine, hashPhase, volumeNoiseAt, rayBands, softBand, layered,
// colorLadder3). The arc waves, the ray salt and the layer stack are the source's look data.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    vec2 center;
    float colorSpace;
    float balance;
    float intensity;
    float curtains;
    float speed;
    float waviness;
    float rayDensity;
    float height;
    float seed;
} params;

vec4 jfx_paint(JfxContext c){
    // LAYERS: time offsets and weights; the per-layer seed offsets are the f32 fround(i * fround(31.7))
    const float OFFSETS[4] = float[4](0.0, 3.1, 6.7, 9.4);
    const float WEIGHTS[4] = float[4](1.0, 0.65, 0.45, 0.3);
    const float SEED_OFFSETS[4] = float[4](0.0, 31.7, 63.4, 95.1000061);
    float t = c.anim.x;
    // the source's centre is stored y-flipped and read back as 1 - y: the authored position
    float centerX = params.center.x;
    float centerY = params.center.y;
    float wav = params.waviness * 0.01;
    float rayAmt = params.rayDensity * 0.01;
    float auroraHeight = params.height * 0.01;
    float intens = params.intensity * 0.018;
    float balanceExp = mix(2.5, 0.4, params.balance * 0.01);
    int count = int(clamp(params.curtains, 1.0, 4.0) + 0.5);

    float weightedSum = 0.0;
    float total = 0.0;
    for (int i = 0; i < 4; i++){
        if (i >= count)
            break;
        float tSlow = t * 0.06 + OFFSETS[i];
        float lSeed = params.seed + SEED_OFFSETS[i];
        float wx = (c.uv.x - centerX) * c.aspect * 2.0;

        // the path: a wavy line (ARC_WAVES) wobbled by the curtain's one noise read
        float arc = sin(wx * 0.7 + tSlow * 0.4 + fam_hash_phase(lSeed, 12.9, 4758.5)) * 0.15
                  + sin(wx * 1.6 + tSlow * 0.65 + fam_hash_phase(lSeed, 78.2, 2847.1)) * 0.08
                  + sin(wx * 2.9 + tSlow * 0.3 + fam_hash_phase(lSeed, 41.6, 1593.7)) * 0.04;
        float noiseWarp = jfx_mx_noise3(vec3(wx * 0.5, tSlow * 0.25, lSeed * 0.1));
        float pathY = centerY + arc * wav + noiseWarp * 0.14 * wav;

        // vertical rays flicker the curtain's height (rayBands, RAY_SALT)
        float rayArg = (wx + noiseWarp * 2.5) * (mix(6.0, 20.0, rayAmt) * 0.15) + tSlow * 0.1 + fam_hash_phase(lSeed, 53.7, 3847.2);
        float rays = pow(sin(rayArg) * 0.5 + 0.5, 1.5);
        float localHeight = auroraHeight * mix(0.4, 1.0, rays);

        // softBand: a sharp fade-in below the path, a long ray-flickered fade-out above it
        float distFromPath = c.uv.y - pathY;
        float topExtent = localHeight * (rays * 0.4 + 0.6);
        float envelope = smoothstep(-0.04, 0.015, distFromPath) * (1.0 - smoothstep(0.0, topExtent, distFromPath));

        float colorT = clamp(distFromPath / (topExtent + 0.001), 0.0, 1.0);
        float sectionBright = mix(0.4, 1.0, noiseWarp * 0.5 + 0.5);
        float coreD = colorT - 0.2;
        float coreBright = exp(coreD * coreD * -6.0) * 0.3 + 1.0;
        float alpha = envelope * rays * sectionBright * coreBright;

        // layered: sum of value * weight and of weight
        float weight = alpha * WEIGHTS[i];
        weightedSum += colorT * weight;
        total += weight;
    }

    // the balance remap and the A -> B -> C ladder; brightness is the accumulated light
    float avgT = pow(weightedSum / (total + 0.001), balanceExp);
    vec4 cAB = jfx_mix_colors(params.colorA, params.colorB, smoothstep(0.0, 0.2, avgT), params.colorSpace);
    vec4 color = jfx_mix_colors(cAB, params.colorC, smoothstep(0.4, 0.85, avgT), params.colorSpace);
    float brightness = total * intens;
    return vec4(clamp(color.rgb * brightness, 0.0, 1.0), clamp(brightness * 0.5, 0.0, 1.0));
}

#include "effects/lib/main_paint.glsl"
