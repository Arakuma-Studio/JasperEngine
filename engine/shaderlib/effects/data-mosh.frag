#version 450
// @effect     data-mosh
// @kind       filter
// @category   stylize
// @summary    Corrupted-codec smearing: macroblocks lock and drag their pixels in liquid trails
// @source     shaders.com DataMosh
// @cost       stateful
// @state      state rgba16f 768x768 linear
// @step       decode state
// @suggest-for datamosh codec-glitch
// @param      intensity number 0.7 0 1 -- How much of the image locks and smears (the share of corrupted blocks)
// @param      block-size number 48 30 150 -- Macroblock size (state texels of the 768 grid)
// @param      drift number 0.35 0 1 -- How fast corrupted areas drag their pixels
// @param      churn number 0.4 0 1 -- How often each block re-rolls its fate
// @param      speed number 1 0 4 -- Simulation speed (0 freezes the decay)
// @param      seed number 0 0 100 -- Random seed of the corruption layout

// Ported from shaders.com DataMosh (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/DataMosh/index.ts, gpu/scaffolds/gridKernels.ts buildMacroblockAdvectKernel,
// std/sim/feedback.ts): a 768 x 768 state holding "the last decoded frame" (premultiplied, as the
// layer); every step each macroblock either refreshes from the live content or holds, advecting its
// previous pixels along its motion vector with generation loss, on per-block clocks (no global
// heartbeat). The sim's own clock is the sum of its speed-scaled steps (dt clamped to 0.05 s).
// The live content is read bilinearly at the state texel (upstream reads its RTT texels).
// Upstream's `blend` (live content vs. the moshed stream) is the universal `opacity` of a filter.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float intensity;
    float blockSize;
    float drift;
    float churn;
    float speed;
    float seed;
} params;

JFX_STATE(state)

const float RES = 768.0;
const float COARSE_SCALE = 2.6;
const float COARSE_PICK = 0.35;
const float QUANT_LEVELS = 18.0;
const float QUANT_AMOUNT = 0.12;
const float ENERGY_DECAY = 0.9985;

vec4 jfx_filter(JfxContext c){
    // the moshed stream (the universal opacity mixes it with the live content, upstream's blend)
    return jfx_unpremultiply(state_sample(c.uv));
}

#ifdef JFX_STEP_FORM
vec4 jfx_step(JfxStep s){
    float dt = min(s.dt, 0.05) * params.speed;
    // the sim's own clock: its speed-scaled steps (localTime)
    float time = s.steps * min(s.dt, 0.05) * params.speed;
    float px = float(s.cell.x);
    float py = float(s.cell.y);
    float fx = (px + 0.5) / RES;
    float fy = (py + 0.5) / RES;
    float churnRate = 0.15 + params.churn * 0.8;
    float bsF = max(params.blockSize, 8.0);
    float coarse = bsF * COARSE_SCALE;
    float cbx = floor(px / coarse);
    float cby = floor(py / coarse);
    float cPhase = jfx_hash11(cbx * 91.17 + cby * 41.23 + params.seed * 3.7);
    float cEpoch = floor(time * churnRate * 0.25 + cPhase);
    float scalePick = jfx_hash11(cbx * 12.9898 + cby * 78.233 + cEpoch * 37.71 + params.seed * 3.7);
    float bs = scalePick < COARSE_PICK ? coarse : bsF;
    float bx = floor(px / bs);
    float by = floor(py / bs);
    float bPhase = jfx_hash11(bx * 127.1 + by * 311.7 + params.seed * 17.9);
    float bRate = churnRate * (0.7 + 0.6 * jfx_hash11(bx * 53.7 + by * 97.3 + params.seed * 5.3));
    float bEpoch = floor(time * bRate + bPhase);
    float bh = bx * 127.1 + by * 311.7 + bEpoch * 74.7 + params.seed * 17.9;
    float h1 = jfx_hash11(bh + 0.13);
    float h2 = jfx_hash11(bh + 7.77);
    float h3 = jfx_hash11(bh + 23.19);
    float bcx = (bx + 0.5) * bs / RES;
    float bcy = (by + 0.5) * bs / RES;
    float field = 0.5 + 0.25 * sin(bcx * 2.7 + time * 0.31 + params.seed) * sin(bcy * 2.1 - time * 0.23 + params.seed * 0.4) +
                  0.25 * sin((bcx + bcy) * 1.6 + time * 0.17 + params.seed * 0.7);
    float thresh = 1.0 - params.intensity * 0.55;
    float hold = smoothstep(thresh - 0.09, thresh + 0.09, field + (h1 - 0.5) * 0.3);
    float flowAng = sin(bcx * 1.9 + params.seed * 3.1) * 1.7 + cos(bcy * 2.3 - params.seed * 2.7) * 1.3 + time * 0.15;
    float ang = flowAng + (h2 - 0.5) * 1.1;
    float mag = (0.5 + 0.5 * h3) * params.drift * dt * 10.0 * (bs / RES);
    float suRaw = fx - cos(ang) * mag;
    float svRaw = fy - sin(ang) * mag;
    float inBounds = (suRaw >= 0.0 && suRaw <= 1.0 && svRaw >= 0.0 && svRaw <= 1.0) ? 1.0 : 0.0;
    float su = clamp(suRaw, 0.0, 1.0) * RES - 0.5;
    float sv = clamp(svRaw, 0.0, 1.0) * RES - 0.5;
    float sx0 = floor(su);
    float sy0 = floor(sv);
    float sfx = su - sx0;
    float sfy = sv - sy0;
    float maxT = RES - 1.0;
    ivec2 i00 = ivec2(clamp(sx0, 0.0, maxT), clamp(sy0, 0.0, maxT));
    ivec2 i11 = ivec2(clamp(sx0 + 1.0, 0.0, maxT), clamp(sy0 + 1.0, 0.0, maxT));
    vec4 p00 = state_fetch(ivec2(i00.x, i00.y));
    vec4 p10 = state_fetch(ivec2(i11.x, i00.y));
    vec4 p01 = state_fetch(ivec2(i00.x, i11.y));
    vec4 p11 = state_fetch(ivec2(i11.x, i11.y));
    vec4 prevC = mix(mix(p00, p10, sfx), mix(p01, p11, sfx), sfy);
    vec3 quant = (floor(prevC.xyz * QUANT_LEVELS) + 0.5) / QUANT_LEVELS;
    vec4 held = vec4(mix(prevC.xyz, quant, QUANT_AMOUNT) * ENERGY_DECAY, prevC.w);
    float rekey = clamp(dt * (1.2 + (1.0 - params.intensity) * 2.0), 0.0, 0.2);
    // the live content at this texel (premultiplied)
    vec4 fresh = jfx_premultiply(jfx_src(vec2(fx, fy) * s.c.size));
    // the injected splats corrupt the blocks under them (held, as at full intensity)
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 d = vec2(bcx, bcy) * s.c.size - sp.pos;
        hold = max(hold, sp.strength * (1.0 - smoothstep(sp.radius * 0.5, sp.radius, length(d))));
    }
    vec4 heldAnchored = mix(held, fresh, rekey);
    return mix(fresh, heldAnchored, hold * inBounds);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
