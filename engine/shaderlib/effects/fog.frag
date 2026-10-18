#version 450
// @effect     fog
// @kind       paint
// @category   interactive
// @summary    A seamless fluid fog that drifts and churns in two colours; the pointer pushes it
// @source     shaders.com Fog
// @cost       stateful
// @pointer    trail
// @state      vel rgba16f 256x256 nearest
// @state      pd rgba16f 256x256 nearest
// @state      dye rgba16f 256x256 linear
// @init       seed dye
// @warmup     50 0.1s
// @reseed     seed
// @step       force vel
// @step       curl vel
// @step       vorticity vel
// @step       divergence pd
// @step       jacobi pd x10
// @step       project vel
// @step       advect-vel vel
// @step       advect-dye dye
// @step       restore dye
// @suggest-for mist fluid-fog
// @param      color-a color #e0e0e0 -- Primary fog colour
// @param      color-b color #888888 -- Secondary colour, varying across the field
// @param      seed number 0 0 999 -- Starting pattern: each seed is another fog
// @param      speed number 1 0.1 3 -- Simulation speed
// @param      turbulence number 1 0 3 -- Ambient motion strength
// @param      detail number 15 0 50 -- Fine-scale swirling structure (vorticity confinement)
// @param      blending number 0.3 0 1 -- 0 keeps the two colours distinct like oil and water, 1 mixes them fully
// @param      mouse-influence number 0.1 0 2 -- Strength of the pointer's push
// @param      mouse-radius number 0.1 0.02 0.5 -- Radius of the pointer's push (share of the area)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the two-colour mix

// Ported from shaders.com Fog (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/Fog/index.ts, gpu/scaffolds/fluids.ts buildNoiseFieldInitKernel,
// buildTrigTurbulenceKernel, buildNoiseRestoreKernel, std/sim/fluids.ts seededFieldInit): a
// toroidal 256 x 256 Stable-Fluids solve over a permanent density + colour-variation field,
// seeded from fBm, warmed up silently (50 steps of 0.1 s, 6 Jacobi iterations), stirred by the
// analytic ambient wind and the pointer, and pulled back toward its seed pattern after every
// solve. The warm-up's random start time is a hash of the seed and the element.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float seed;
    float speed;
    float turbulence;
    float detail;
    float blending;
    float mouseInfluence;
    float mouseRadius;
    float colorSpace;
} params;

JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(dye)

#include "effects/lib/fam_sim_fluid.glsl"

const float VELOCITY_DISSIPATION = 0.15;
const float NOISE_FREQ = 3.0;

vec4 jfx_paint(JfxContext c){
    vec4 s = dye_sample(c.uv);
    vec4 color = jfx_mix_colors(params.colorA, params.colorB, clamp(s.y, 0.0, 1.0), params.colorSpace);
    return vec4(color.rgb, color.a * clamp(s.x, 0.0, 1.0));
}

#ifdef JFX_STEP_FORM
// the fog's clock: the simulation time from a seed- and element-hashed start (upstream: random)
float fog_time(JfxStep s){
    return s.time + jfx_hash11(params.seed * 1.618 + s.c.seed * 977.0) * 60.0;
}

float fog_dt(JfxStep s){
    // the warm-up's steps are 0.1 s each, without the speed (upstream's warm values)
    return s.warm ? s.dt : s.dt * params.speed;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    float n = float(fluid_n());
    float dt = fog_dt(s);
    if (s.pass == JFX_PASS_SEED){
        // buildNoiseFieldInitKernel (density scale 0.75, bias 0.25, 0.15..0.9)
        vec2 lat = fluid_seed_lattice(float(cell.x), float(cell.y), params.seed, NOISE_FREQ / n);
        float dens0 = clamp(fluid_fbm(lat.x + 31.7, lat.y + 53.3) * 0.75 + 0.25, 0.15, 0.9);
        float colorVar = clamp(fluid_fbm(lat.x, lat.y), 0.0, 1.0);
        return vec4(dens0, colorVar, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_FORCE){
        // buildTrigTurbulenceKernel (gain N x 0.04) + the cursor shove
        float gain = n * 0.04;
        float jf = float(cell.x) + 0.5;
        float iF = float(cell.y) + 0.5;
        float x = jf / n * JFX_TAU;
        float y = iF / n * JFX_TAU;
        float t = fog_time(s);
        vec4 v = vel_fetch(cell);
        float fx = (sin(x * 2.0 + t * 0.31) * cos(y * 1.5 + t * 0.23) + sin(x * 0.7 + y * 1.3 + t * 0.17) * 0.4) * params.turbulence * gain * dt;
        float fy = (cos(x * 1.5 + t * 0.27) * sin(y * 2.0 + t * 0.19) + cos(x * 1.1 + y * 0.9 + t * 0.21) * 0.4) * params.turbulence * gain * dt;
        // the pointer (minDrag 0.0005, the 0.25 teleport guard); none during the warm-up
        vec2 size = max(s.c.size, vec2(1.0));
        vec2 delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / size : vec2(0.0);
        float dragDist = length(delta);
        bool active = !s.warm && s.pointerKnown && dragDist < 0.25 && dragDist > 0.0005 && params.mouseInfluence > 0.0;
        vec2 cursor = active ? s.pointer / size * n : vec2(0.0);
        vec2 cursorVel = active ? delta * n * 15.0 * params.mouseInfluence : vec2(0.0);
        float mouseRadGrid = params.mouseRadius * n;
        float mouseRadSq = active ? mouseRadGrid * mouseRadGrid : 1.0;
        float cdx = jf - cursor.x;
        float cdy = iF - cursor.y;
        float cursorInf = fluid_brush(cdx * cdx + cdy * cdy, max(mouseRadSq, 1.0)) * (active ? 1.0 : 0.0);
        vec2 add = vec2(fx, fy) + cursorVel * cursorInf;
        // the injected splats push the fog
        vec2 cellsPerPx = n / size;
        for (int i = 0; i < 8; i++){
            if (i >= s.splatCount)
                break;
            JfxSplat sp = jfx_splat(i);
            vec2 d = vec2(jf, iF) - sp.pos * cellsPerPx;
            float r = max(sp.radius * cellsPerPx.x, 1.0);
            add += sp.vel * cellsPerPx * sp.strength * fluid_brush(dot(d, d), r * r);
        }
        return vec4(v.x + add.x, v.y + add.y, v.z, 0.0);
    }
    if (s.pass == JFX_PASS_CURL)
        return fluid_curl(cell, true);
    if (s.pass == JFX_PASS_VORTICITY)
        return fluid_vorticity(cell, true, params.detail, dt, 0.0, 0.0, 0.0, 0.0);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return fluid_divergence(cell, true, 0.8);
    if (s.pass == JFX_PASS_JACOBI){
        // the warm-up relaxes 6 times a step
        if (s.warm && s.iteration >= 6)
            return pd_fetch(cell);
        return fluid_jacobi(cell, true);
    }
    if (s.pass == JFX_PASS_PROJECT)
        return fluid_project(cell, true, n * 0.6);
    if (s.pass == JFX_PASS_ADVECT_VEL)
        return fluid_advect_vel(cell, true, dt, VELOCITY_DISSIPATION);
    if (s.pass == JFX_PASS_ADVECT_DYE)
        return fluid_advect_density_age(cell, true, dt, false, 0.0, 0.0);
    // buildNoiseRestoreKernel: the variation channel back toward the seed pattern
    vec2 lat = fluid_seed_lattice(float(cell.x), float(cell.y), params.seed, NOISE_FREQ / n);
    float refRaw = fluid_fbm(lat.x, lat.y);
    float invBlend = 1.0 - params.blending;
    float contrastBoost = 1.0 + invBlend * 5.0;
    float refBoosted = clamp((refRaw - 0.5) * contrastBoost + 0.5, 0.0, 1.0);
    vec4 d = dye_fetch(cell);
    float rateMult = 1.0 + invBlend * 2.5;
    float blendRate = min(dt * 0.5 * rateMult, 0.3);
    return vec4(d.x, mix(d.y, refBoosted, blendRate), 0.0, 0.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
