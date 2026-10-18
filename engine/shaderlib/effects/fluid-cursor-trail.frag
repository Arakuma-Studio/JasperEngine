#version 450
// @effect     fluid-cursor-trail
// @kind       paint
// @category   interactive
// @summary    A trail of fluid paint behind the pointer, ageing from the fresh to the old colour
// @source     shaders.com CursorTrail + SmokeFlow (adapted: CursorTrail's look over SmokeFlow's fluid)
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
// @suggest-for fluid-trail ink-trail
// @param      colors palette "#00aaff #ff00aa" -- Colours of the trail from fresh to old
// @param      radius number 0.5 0.5 2 -- Radius of the stroke (1 is a tenth of the area)
// @param      length time 0.5s 0.1s 2s -- How long the trail lasts
// @param      shrink number 1 0 1 -- How much the stroke thins as it fades (0 none)
// @param      softness number 0 0 1 -- Edge softness (0 is a crisp paint edge)
// @param      momentum number 12 0 50 -- How much of the pointer's velocity the paint carries
// @param      swirl number 10 0 60 -- Swirl detail of the flow (vorticity confinement)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the ramp

// After shaders.com CursorTrail and SmokeFlow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt.
// Upstream's CursorTrail is a CPU polyline drawn analytically (phase 5's `cursor-trail` ports it);
// this is its fluid version: SmokeFlow's ribbon of gated puffs along the pointer's path over the
// free-slip Stable-Fluids solve (shaders/SmokeFlow/index.ts, gpu/scaffolds/fluids.ts), with
// CursorTrail's parameters and look. The puff radius is radius x 0.1 of the area (CursorTrail's
// drawRadius) times the pen-pressure width of the smoothed speed (sqrt(speed / 1.5), at least
// 0.2); the age runs from 0 to 1 over `length`, the density fades to 1/255 over it
// (decayFadeSeconds), and the colour ramp reads the age. `shrink` thins the coverage with age,
// `softness` widens the coverage edge.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    float colorsCount;
    float radius;
    float length;
    float shrink;
    float softness;
    float momentum;
    float swirl;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

JFX_STATE(ptr)
JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(dye)

#include "effects/lib/fam_sim_fluid.glsl"

const int MAX_STEPS = 16;
const float WIDTH_MIN = 0.2;
const float SPEED_FULL = 1.5;

vec4 jfx_paint(JfxContext c){
    vec4 s = dye_sample(c.uv);
    float age = clamp(s.y, 0.0, 1.0);
    float density = clamp(s.x, 0.0, 1.0) * (1.0 - age * params.shrink);
    // a crisp paint edge (softness 0) or the density as it is (1)
    float edge = mix(0.02, 0.5, params.softness);
    float coverage = smoothstep(0.08 * (1.0 - params.softness), 0.08 * (1.0 - params.softness) + edge, density);
    vec4 color = jfx_palette_colors(age, params.colorSpace);
    return vec4(color.rgb, color.a * coverage * (1.0 - smoothstep(0.8, 1.0, age)));
}

#ifdef JFX_STEP_FORM
vec2 trail_delta(JfxStep s){
    return s.pointerKnown ? (s.pointer - s.pointerPrev) / max(s.c.size, vec2(1.0)) : vec2(0.0);
}

vec4 trail_splat(JfxStep s, bool velocityPass){
    float n = float(fluid_n());
    ivec2 cell = s.cell;
    vec4 d = dye_fetch(cell);
    vec4 v = vel_fetch(cell);
    vec2 smoothVel = ptr_fetch(ivec2(0)).xy;
    float smoothSpeed = length(smoothVel);
    vec2 delta = trail_delta(s);
    // any movement paints (CursorTrail's minDrag 0.001, no teleport guard)
    bool active = s.pointerKnown && length(delta) > 0.001;
    float width = min(1.0, max(WIDTH_MIN, sqrt(smoothSpeed / SPEED_FULL)));
    float emitRad = max(params.radius * 0.1 * n * width * 0.5, 1.0);
    vec2 emitVel = smoothVel * n * params.momentum * 0.12;
    if (active){
        vec2 from = s.pointerPrev / max(s.c.size, vec2(1.0));
        float stepSize = max(0.004, params.radius * 0.1 * 0.5 * width);
        int steps = fluid_ribbon_steps(length(delta), stepSize, MAX_STEPS);
        for (int k = 0; k < MAX_STEPS; k++){
            if (k >= steps)
                break;
            float t = (float(k) + 0.5) / float(steps);
            vec2 emit = (from + delta * t) * n;
            if (velocityPass)
                v = fluid_puff_vel(cell, emit, emitVel, emitRad, 1.0, s.dt, 4.0, 0.0, d.x, v);
            d = fluid_puff_dye(cell, emit, emitRad, 1.0, 1.0, s.dt, 60.0, d);
        }
    }
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        if (velocityPass)
            v = fluid_puff_vel(cell, sp.pos * cellsPerPx, sp.vel * cellsPerPx, r, 1.0, 1.0, 1.0, 0.0, d.x, v);
        d = fluid_puff_dye(cell, sp.pos * cellsPerPx, r, 1.0, sp.strength, 1.0, 1.0, d);
    }
    return velocityPass ? v : d;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    float lengthS = max(params.length, 0.05);
    if (s.pass == JFX_PASS_TRACK){
        vec2 velocity = trail_delta(s) / max(s.dt, 0.001);
        return vec4(ptr_fetch(ivec2(0)).xy * (1.0 - 0.2) + velocity * 0.2, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_SPLAT_VEL)
        return trail_splat(s, true);
    if (s.pass == JFX_PASS_SPLAT_DYE)
        return trail_splat(s, false);
    if (s.pass == JFX_PASS_CURL)
        return fluid_curl(cell, false);
    if (s.pass == JFX_PASS_VORTICITY)
        return fluid_vorticity(cell, false, params.swirl, s.dt, 0.0, 0.0, 0.0, 0.0);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return fluid_divergence(cell, false, 0.8);
    if (s.pass == JFX_PASS_JACOBI)
        return fluid_jacobi(cell, false);
    if (s.pass == JFX_PASS_PROJECT)
        return fluid_project(cell, false, 0.0);
    if (s.pass == JFX_PASS_ADVECT_VEL)
        return fluid_advect_vel(cell, false, s.dt, 0.2);
    // the density fades to 1/255 over `length` (decayFadeSeconds(255, rate)), the age runs 0..1
    // over it (colorDecay x 0.4 per second is 1 / length)
    float dyeFade = log(255.0) / lengthS;
    return fluid_advect_density_age(cell, false, s.dt, true, dyeFade, 1.0 / (0.4 * lengthS));
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
