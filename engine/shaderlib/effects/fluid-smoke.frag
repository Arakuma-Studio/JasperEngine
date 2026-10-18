#version 450
// @effect     fluid-smoke
// @kind       paint
// @category   interactive
// @summary    Fluid smoke rising from a source, swirling with vorticity; the pointer pushes it
// @source     shaders.com Smoke
// @cost       stateful
// @pointer    trail
// @state      vel rgba16f 256x256 nearest
// @state      pd rgba16f 256x256 nearest
// @state      dye rgba16f 256x256 linear
// @step       splat-vel vel
// @step       splat-dye dye
// @step       curl vel
// @step       vorticity vel
// @step       divergence pd
// @step       jacobi pd x10
// @step       project vel
// @step       advect-vel vel
// @step       advect-dye dye
// @suggest-for smoke-simulation fluid
// @param      colors palette "#fc83f9 #c21c79" -- Colours of the smoke from fresh to aged
// @param      emit-from position "50% 100%" -- The emission source
// @param      direction angle 0deg 0deg 360deg -- Emission direction (0deg up, 90deg right)
// @param      speed number 20 0.1 50 -- Emission velocity strength
// @param      spread angle 60deg 0deg 180deg -- Emission cone angle
// @param      emit-radius number 0.08 0.01 0.3 -- Size of the emission area (share of the area)
// @param      intensity number 1 0.1 1 -- Smoke emission density
// @param      dissipation number 0.2 0.1 3 -- How fast smoke fades over time
// @param      detail number 25 0 50 -- Fine-scale swirling detail (vorticity confinement)
// @param      gravity number 0.5 -2 2 -- Downward pull on the smoke (negative rises)
// @param      color-decay number 0.4 0 3 -- How quickly smoke shifts from the fresh to the aged colour
// @param      mouse-influence number 0.1 0 2 -- Strength of the pointer's push
// @param      mouse-radius number 0.1 0.02 0.5 -- Radius of the pointer's push (share of the area)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the colour ramp

// Ported from shaders.com Smoke (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/Smoke/index.ts, gpu/scaffolds/fluids.ts): the 256 x 256 free-slip Stable-Fluids solve
// over a density + age dye, the cone emitter with the cursor shove, the colour ramp by age. The
// pointer tracker (minDrag 0.0005, the 0.25 teleport guard) works on the step's pointer.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 colors[8];
    vec4 colorsStops[2];
    vec2 emitFrom;
    float colorsCount;
    float direction;
    float speed;
    float spread;
    float emitRadius;
    float intensity;
    float dissipation;
    float detail;
    float gravity;
    float colorDecay;
    float mouseInfluence;
    float mouseRadius;
    float colorSpace;
} params;

JFX_DECLARE_PALETTE(colors)

JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(dye)

#include "effects/lib/fam_sim_fluid.glsl"

const float VELOCITY_DISSIPATION = 0.2;

vec4 jfx_paint(JfxContext c){
    vec4 s = dye_sample(c.uv);
    float density = s.x;
    float age = s.y;
    vec4 color = jfx_palette_colors(clamp(age, 0.0, 1.0), params.colorSpace);
    return vec4(color.rgb, color.a * clamp(density, 0.0, 1.0));
}

#ifdef JFX_STEP_FORM
// Smoke's values(f), per step
FluidEmitter smoke_emitter(JfxStep s){
    float n = float(fluid_n());
    FluidEmitter e;
    e.dt = s.dt;
    float dirRad = params.direction;
    float spreadHalf = min(radians(89.0), params.spread * 0.5);
    float velMag = params.speed * n * 0.15;
    e.emit = params.emitFrom * n;
    e.emitVel = vec2(sin(dirRad) * velMag, -cos(dirRad) * velMag);
    e.perpDir = vec2(cos(dirRad), sin(dirRad));
    e.spreadFactor = tan(spreadHalf);
    e.emitRad = params.emitRadius * n;
    e.emitIntensity = params.intensity;
    e.gravity = params.gravity;
    // the pointer tracker: the step's movement in uv, the teleport guard, the minimum drag
    vec2 size = max(s.c.size, vec2(1.0));
    vec2 delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / size : vec2(0.0);
    float dragDist = length(delta);
    bool teleport = dragDist >= 0.25;
    bool moving = s.pointerKnown && !teleport && dragDist > 0.0005;
    bool isActive = moving && params.mouseInfluence > 0.0;
    e.cursor = isActive ? s.pointer / size * n : vec2(0.0);
    e.cursorVel = isActive ? delta * n * 15.0 * params.mouseInfluence : vec2(0.0);
    e.mouseActive = isActive ? 1.0 : 0.0;
    float mouseRadGrid = params.mouseRadius * n;
    e.mouseRadSq = mouseRadGrid * mouseRadGrid;
    e.densityGain = 8.0;
    e.velocityBlendGain = 3.0;
    return e;
}

// the injected splats (UIElement::effectSplat): density and momentum around each
vec4 smoke_splat_dye(JfxStep s, vec4 d){
    float n = float(fluid_n());
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    vec2 cell = vec2(s.cell) + 0.5;
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 dlt = cell - sp.pos * cellsPerPx;
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        float add = sp.strength * fluid_brush(dot(dlt, dlt), r * r);
        float newDens = min(d.x + add, 1.0);
        d = vec4(newDens, d.y * (1.0 - min(add / max(newDens, 0.001), 1.0)), 0.0, 0.0);
    }
    return d;
}

vec4 smoke_splat_vel(JfxStep s, vec4 v){
    float n = float(fluid_n());
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    vec2 cell = vec2(s.cell) + 0.5;
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 dlt = cell - sp.pos * cellsPerPx;
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        v.xy += sp.vel * cellsPerPx * fluid_brush(dot(dlt, dlt), r * r);
    }
    return v;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_SPLAT_VEL){
        vec4 v = fluid_emit_vel(cell, smoke_emitter(s), vel_fetch(cell), dye_fetch(cell).x);
        return smoke_splat_vel(s, v);
    }
    if (s.pass == JFX_PASS_SPLAT_DYE)
        return smoke_splat_dye(s, fluid_emit_dye(cell, smoke_emitter(s), dye_fetch(cell)));
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
