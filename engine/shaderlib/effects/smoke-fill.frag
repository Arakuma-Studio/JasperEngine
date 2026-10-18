#version 450
// @effect     smoke-fill
// @kind       paint
// @category   interactive
// @summary    Fluid smoke filling the element's rounded box, piling up against its edges
// @source     shaders.com SmokeFill (adapted: the shape is the element's rounded box)
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
// @suggest-for contained-smoke
// @param      colors palette "#8cf3ff #04a0d6" -- Colours of the smoke from fresh to aged
// @param      emit-from position "50% 50%" -- The emission source inside the box
// @param      direction angle 0deg 0deg 360deg -- Emission direction (0deg up, 90deg right)
// @param      speed number 10 0.1 30 -- Emission velocity strength
// @param      spread angle 60deg 0deg 180deg -- Emission cone angle
// @param      emit-radius number 0.03 0.01 0.3 -- Size of the emission area (share of the area)
// @param      intensity number 1 0.1 1 -- Smoke emission density
// @param      dissipation number 0.3 0.1 5 -- How fast smoke fades over time
// @param      detail number 25 0 50 -- Fine-scale swirling detail (vorticity confinement)
// @param      gravity number 0.5 -2 2 -- Downward pull on the smoke (negative rises)
// @param      color-decay number 0.4 0 3 -- How quickly smoke shifts from the fresh to the aged colour
// @param      mouse-influence number 0.1 0 2 -- Strength of the pointer's push
// @param      mouse-radius number 0.1 0.02 0.5 -- Radius of the pointer's push (share of the area)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the colour ramp

// Ported from shaders.com SmokeFill (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/SmokeFill/index.ts, gpu/scaffolds/fluids.ts with solidMask): the 256 x 256 Stable-Fluids
// solve confined to a shape (cells outside it are solid walls; emission, gravity, the cursor and
// the advected fields are masked), the cone emitter, the colour ramp by age. As phase 5's shape
// effects, the shape is the element's rounded box (the grid covers the area): its center, scale,
// rotation and shape props are not ported; the paint form's rounded-box clip is the crisp edge.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sdf.glsl"
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

// the solid mask: inside the element's rounded box (the cell's centre in the area)
#define FLUID_MASKED 1
float fluid_mask(ivec2 cell){
    vec2 n = vec2(vel_size());
    vec2 px = (vec2(cell) + 0.5) / n * frame.size.xy;
    return jfx_box_sdf(px, frame.size.xy, frame.radii) < 0.0 ? 1.0 : 0.0;
}

#include "effects/lib/fam_sim_fluid.glsl"

const float VELOCITY_DISSIPATION = 0.2;

vec4 jfx_paint(JfxContext c){
    vec4 s = dye_sample(c.uv);
    vec4 color = jfx_palette_colors(clamp(s.y, 0.0, 1.0), params.colorSpace);
    return vec4(color.rgb, color.a * clamp(s.x, 0.0, 1.0));
}

#ifdef JFX_STEP_FORM
FluidEmitter fill_emitter(JfxStep s){
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
    vec2 size = max(s.c.size, vec2(1.0));
    vec2 delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / size : vec2(0.0);
    float dragDist = length(delta);
    bool isActive = s.pointerKnown && dragDist < 0.25 && dragDist > 0.0005 && params.mouseInfluence > 0.0;
    e.cursor = isActive ? s.pointer / size * n : vec2(0.0);
    e.cursorVel = isActive ? delta * n * 15.0 * params.mouseInfluence : vec2(0.0);
    e.mouseActive = isActive ? 1.0 : 0.0;
    float mouseRadGrid = params.mouseRadius * n;
    e.mouseRadSq = mouseRadGrid * mouseRadGrid;
    e.densityGain = 8.0;
    e.velocityBlendGain = 3.0;
    return e;
}

vec4 fill_splats(JfxStep s, vec4 value, bool velocity){
    float n = float(fluid_n());
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    vec2 cell = vec2(s.cell) + 0.5;
    float m = fluid_mask(s.cell);
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 d = cell - sp.pos * cellsPerPx;
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        float w = fluid_brush(dot(d, d), r * r) * m;
        if (velocity){
            value.xy += sp.vel * cellsPerPx * w;
        }else{
            float add = sp.strength * w;
            float newDens = min(value.x + add, 1.0);
            value = vec4(newDens, value.y * (1.0 - min(add / max(newDens, 0.001), 1.0)), 0.0, 0.0);
        }
    }
    return value;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_SPLAT_VEL)
        return fill_splats(s, fluid_emit_vel(cell, fill_emitter(s), vel_fetch(cell), dye_fetch(cell).x), true);
    if (s.pass == JFX_PASS_SPLAT_DYE)
        return fill_splats(s, fluid_emit_dye(cell, fill_emitter(s), dye_fetch(cell)), false);
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
