#version 450
// @effect     smoke-flow
// @kind       paint
// @category   interactive
// @summary    Smoke the pointer leaves behind as it moves, lingering and swirling as a fluid
// @source     shaders.com SmokeFlow
// @cost       stateful
// @pointer    trail
// @state      ptr rgba32f 1x1 nearest
// @state      vel rgba16f 256x256 nearest
// @state      pd rgba16f 256x256 nearest
// @state      dye rgba16f 256x256 linear
// @step       track ptr
// @step       splat-vel vel
// @step       splat-dye dye
// @step       curl vel
// @step       vorticity vel
// @step       divergence pd
// @step       jacobi pd x10
// @step       project vel
// @step       advect-vel vel
// @step       advect-dye dye
// @suggest-for cursor-smoke smoke-trail
// @param      colors palette "#e29c8b #d517f9" -- Colours of the smoke from fresh to aged
// @param      intensity number 1 0.1 2 -- How much smoke the moving pointer emits
// @param      emit-radius number 0.07 0.01 0.3 -- Size of a puff (share of the area)
// @param      momentum number 20 0 50 -- How much of the pointer's velocity goes into the smoke
// @param      dissipation number 0.5 0.05 3 -- How fast the smoke fades
// @param      detail number 10 0 60 -- Fine-scale swirling detail (vorticity confinement)
// @param      gravity number 2 -5 5 -- Vertical drift (negative floats up, positive sinks)
// @param      color-decay number 0.5 0 3 -- How quickly the smoke shifts from the fresh to the aged colour
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the colour ramp

// Ported from shaders.com SmokeFlow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/SmokeFlow/index.ts, gpu/scaffolds/fluids.ts, std/sim/fluids.ts cursorRibbon,
// gpu/kit/host/pointer.ts): the gated puff emitter laid as a ribbon of stamps along the pointer's
// path (16 at most a step), its emission gated by the smoothed speed squared, over the free-slip
// Stable-Fluids solve. The tracker (smoothing 0.2, no teleport guard) keeps its smoothed velocity
// in the 1x1 state `ptr` (uv/s).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float intensity;
    float emitRadius;
    float momentum;
    float dissipation;
    float detail;
    float gravity;
    float colorDecay;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

JFX_STATE(ptr)
JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(dye)

#include "effects/lib/fam_sim_fluid.glsl"

const float VELOCITY_DISSIPATION = 0.2;
const int MAX_STEPS = 16;

vec4 jfx_paint(JfxContext c){
    vec4 s = dye_sample(c.uv);
    vec4 color = jfx_palette_colors(clamp(s.y, 0.0, 1.0), params.colorSpace);
    return vec4(color.rgb, color.a * clamp(s.x, 0.0, 1.0));
}

#ifdef JFX_STEP_FORM
// the step's pointer delta in uv (the tracker: no teleport guard)
vec2 flow_delta(JfxStep s){
    return s.pointerKnown ? (s.pointer - s.pointerPrev) / max(s.c.size, vec2(1.0)) : vec2(0.0);
}

float flow_speed_gate(float smoothSpeed){
    return min(smoothSpeed * smoothSpeed * 60.0, 1.0);
}

// the stamps of this step, applied in order to the dye (and the velocity: velocity pass)
vec4 flow_splat(JfxStep s, bool velocityPass){
    float n = float(fluid_n());
    ivec2 cell = s.cell;
    vec4 d = dye_fetch(cell);
    vec4 v = vel_fetch(cell);
    vec2 smoothVel = ptr_fetch(ivec2(0)).xy;
    float gate = flow_speed_gate(length(smoothVel));
    bool active = s.pointerKnown && gate > 0.005;
    vec2 delta = flow_delta(s);
    float emitRad = params.emitRadius * n;
    vec2 emitVel = smoothVel * n * params.momentum * 0.12;
    if (active){
        vec2 from = s.pointerPrev / max(s.c.size, vec2(1.0));
        float stepSize = max(0.004, params.emitRadius * 0.5);
        int steps = fluid_ribbon_steps(length(delta), stepSize, MAX_STEPS);
        for (int k = 0; k < MAX_STEPS; k++){
            if (k >= steps)
                break;
            float t = (float(k) + 0.5) / float(steps);
            vec2 emit = (from + delta * t) * n;
            if (velocityPass)
                v = fluid_puff_vel(cell, emit, emitVel, emitRad, gate, s.dt, 4.0, params.gravity, d.x, v);
            d = fluid_puff_dye(cell, emit, emitRad, gate, params.intensity, s.dt, 10.0, d);
        }
    }
    // the injected splats: puffs of their strength, with their velocity
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        // density `strength` at the centre, the velocity set there (a blend of rate 1 for a unit step)
        if (velocityPass)
            v = fluid_puff_vel(cell, sp.pos * cellsPerPx, sp.vel * cellsPerPx, r, 1.0, 1.0, 1.0, 0.0, d.x, v);
        d = fluid_puff_dye(cell, sp.pos * cellsPerPx, r, 1.0, sp.strength, 1.0, 1.0, d);
    }
    return velocityPass ? v : d;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_TRACK){
        // createPointerVelocityTracker: the raw velocity of the step, smoothed by 0.2
        vec2 velocity = flow_delta(s) / max(s.dt, 0.001);
        vec2 smoothVel = ptr_fetch(ivec2(0)).xy * (1.0 - 0.2) + velocity * 0.2;
        return vec4(smoothVel, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_SPLAT_VEL)
        return flow_splat(s, true);
    if (s.pass == JFX_PASS_SPLAT_DYE)
        return flow_splat(s, false);
    if (s.pass == JFX_PASS_CURL)
        return fluid_curl(cell, false);
    if (s.pass == JFX_PASS_VORTICITY)
        return fluid_vorticity(cell, false, params.detail, s.dt, 0.0, 0.0, 0.0, 0.0);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return fluid_divergence(cell, false, 0.8);
    if (s.pass == JFX_PASS_JACOBI)
        return fluid_jacobi(cell, false);
    if (s.pass == JFX_PASS_PROJECT)
        return fluid_project(cell, false, 0.0);
    if (s.pass == JFX_PASS_ADVECT_VEL)
        return fluid_advect_vel(cell, false, s.dt, VELOCITY_DISSIPATION);
    return fluid_advect_density_age(cell, false, s.dt, true, params.dissipation, params.colorDecay);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
