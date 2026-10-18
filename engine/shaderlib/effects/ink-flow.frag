#version 450
// @effect     ink-flow
// @kind       paint
// @category   interactive
// @summary    Drag to paint swirling ribbons of ink through a fluid; eddies keep evolving after
// @source     shaders.com InkFlow
// @cost       stateful
// @pointer    trail
// @state      vel rgba16f 256x256 nearest
// @state      pd rgba16f 256x256 nearest
// @state      dye rgba16f 256x256 linear
// @state      cycle rgba32f 1x1 nearest
// @step       splat-vel vel
// @step       splat-dye dye
// @step       cycle cycle
// @step       curl vel
// @step       vorticity vel
// @step       divergence pd
// @step       jacobi pd x10
// @step       project vel
// @step       advect-vel vel
// @step       advect-dye dye
// @suggest-for paint-fluid ink
// @param      color-mode enum rainbow rainbow|single|custom -- How the brush colour is chosen: a cycling rainbow, one colour, or a three-colour cycle
// @param      color-speed number 1 0 4 -- How quickly the brush cycles through the rainbow or the custom colours
// @param      color color #ff2d7e -- The brush colour of color-mode single
// @param      color-1 color #4338ff -- First colour of the custom cycle
// @param      color-2 color #ff2d7e -- Second colour of the custom cycle
// @param      color-3 color #19e3ff -- Third colour of the custom cycle
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space the custom colours blend through
// @param      radius number 0.3 0.1 5 -- Size of the ink splat under the pointer (1 is a tenth of the area)
// @param      force number 1 0 3 -- How hard the pointer's motion pushes the fluid
// @param      curl number 0 0 60 -- Swirl: vorticity confinement that spins the ink into eddies
// @param      decay number 0.5 0.05 4 -- How fast the ink fades
// @param      momentum number 0.6 0 1 -- How long the fluid keeps flowing after a stroke

// Ported from shaders.com InkFlow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/InkFlow/index.ts, gpu/scaffolds/fluids.ts buildBrushSplatKernel): the brush ribbon
// (16 stamps at most a step; the newest colour painted over the dye and a velocity-proportional
// impulse), the free-slip Stable-Fluids solve over an RGB dye, the ink's brightness as its
// coverage. The colour cycle (upstream: a CPU counter advanced per frame and per stamp) is the
// 1x1 state `cycle`; the brush colours mix in the colour space with jfx_mix_colors.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec4 color1;
    vec4 color2;
    vec4 color3;
    float colorMode;
    float colorSpeed;
    float colorSpace;
    float radius;
    float force;
    float curl;
    float decay;
    float momentum;
} params;

JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(dye)
JFX_STATE(cycle)

#include "effects/lib/fam_sim_fluid.glsl"

const float IMPULSE_K = 0.16;
const int MAX_STEPS = 16;
const float CYCLE_PER_SPLAT = 0.006;
const float CYCLE_TIME_RATE = 0.06;
const float VEL_FADE_MAX = 4.0;
const float VEL_FADE_MIN = 0.05;
const float GAIN = 1.35;

vec4 jfx_paint(JfxContext c){
    vec3 dye = dye_sample(c.uv).rgb;
    float lum = max(dye.x, max(dye.y, dye.z));
    return vec4(dye, clamp(lum * GAIN, 0.0, 1.0));
}

#ifdef JFX_STEP_FORM
// hueToLinearRgb: the HSV rainbow (full saturation and value), linearised
vec3 ink_hue_rgb(float hue){
    float h = fract(hue);
    float i = floor(h * 6.0);
    float f = h * 6.0 - i;
    float q = 1.0 - f;
    vec3 rgb;
    if (i < 0.5) rgb = vec3(1.0, f, 0.0);
    else if (i < 1.5) rgb = vec3(q, 1.0, 0.0);
    else if (i < 2.5) rgb = vec3(0.0, 1.0, f);
    else if (i < 3.5) rgb = vec3(0.0, q, 1.0);
    else if (i < 4.5) rgb = vec3(f, 0.0, 1.0);
    else rgb = vec3(1.0, 0.0, q);
    return jfx_srgb_to_linear(rgb);
}

// the brush colour at a point of the cycle (brushSplat's three modes)
vec3 ink_brush(float hueCursor){
    if (params.colorMode > 1.5){
        // custom: the closed loop c1 -> c2 -> c3 -> c1
        float h = fract(hueCursor);
        float seg = min(2.0, floor(h * 3.0));
        float t = h * 3.0 - seg;
        vec4 a = seg < 0.5 ? params.color1 : (seg < 1.5 ? params.color2 : params.color3);
        vec4 b = seg < 0.5 ? params.color2 : (seg < 1.5 ? params.color3 : params.color1);
        return jfx_mix_colors(vec4(a.rgb, 1.0), vec4(b.rgb, 1.0), t, params.colorSpace).rgb;
    }
    if (params.colorMode > 0.5)
        return params.color.rgb;
    return ink_hue_rgb(hueCursor);
}

// the step's stroke: moving (the tracker's minDrag 0.0006 and 0.25 teleport guard), its delta in uv
bool ink_moving(JfxStep s, out vec2 delta){
    delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / max(s.c.size, vec2(1.0)) : vec2(0.0);
    float dragDist = length(delta);
    bool teleport = dragDist >= 0.25;
    if (teleport)
        delta = vec2(0.0);
    return s.pointerKnown && !teleport && dragDist > 0.0006;
}

int ink_steps(JfxStep s, vec2 delta, float radiusGrid){
    float n = float(fluid_n());
    return fluid_ribbon_steps(length(delta), max(0.004, (radiusGrid / n) * 0.6), MAX_STEPS);
}

vec4 ink_splat(JfxStep s, bool velocityPass){
    float n = float(fluid_n());
    ivec2 cell = s.cell;
    vec4 d = dye_fetch(cell);
    vec4 v = vel_fetch(cell);
    vec2 delta;
    bool moving = ink_moving(s, delta);
    float radiusGrid = max(params.radius * 0.1 * n, 1.0);
    if (moving){
        // the cycle drifted with the step's time before the stamps (onFrame), then one advance a stamp
        float hue = cycle_fetch(ivec2(0)).x + s.dt * CYCLE_TIME_RATE * params.colorSpeed;
        vec2 velocity = delta / max(s.dt, 0.001) * n * params.force * IMPULSE_K;
        vec2 from = s.pointerPrev / max(s.c.size, vec2(1.0));
        int steps = ink_steps(s, delta, radiusGrid);
        for (int k = 0; k < MAX_STEPS; k++){
            if (k >= steps)
                break;
            hue += CYCLE_PER_SPLAT * params.colorSpeed;
            float t = (float(k) + 0.5) / float(steps);
            vec2 pos = (from + delta * t) * n;
            if (velocityPass)
                v = fluid_impulse(cell, pos, velocity, radiusGrid, v);
            else
                d = fluid_brush_dye(cell, pos, radiusGrid, 1.0, ink_brush(hue), d);
        }
    }
    // the injected splats: their colour painted, their velocity added
    vec2 cellsPerPx = n / max(s.c.size, vec2(1.0));
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        float r = max(sp.radius * cellsPerPx.x, 1.0);
        if (velocityPass)
            v = fluid_impulse(cell, sp.pos * cellsPerPx, sp.vel * cellsPerPx, r, v);
        else
            d = fluid_brush_dye(cell, sp.pos * cellsPerPx, r, sp.strength, sp.color.rgb, d);
    }
    return velocityPass ? v : d;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    float velFade = VEL_FADE_MAX * pow(VEL_FADE_MIN / VEL_FADE_MAX, clamp(params.momentum, 0.0, 1.0));
    if (s.pass == JFX_PASS_SPLAT_VEL)
        return ink_splat(s, true);
    if (s.pass == JFX_PASS_SPLAT_DYE)
        return ink_splat(s, false);
    if (s.pass == JFX_PASS_CYCLE){
        vec2 delta;
        bool moving = ink_moving(s, delta);
        float n = float(fluid_n());
        float stamps = moving ? float(ink_steps(s, delta, max(params.radius * 0.1 * n, 1.0))) : 0.0;
        float hue = cycle_fetch(ivec2(0)).x + s.dt * CYCLE_TIME_RATE * params.colorSpeed + stamps * CYCLE_PER_SPLAT * params.colorSpeed;
        // kept in [0, 1): the cycle is periodic
        return vec4(fract(hue), 0.0, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_CURL)
        return fluid_curl(cell, false);
    if (s.pass == JFX_PASS_VORTICITY)
        return fluid_vorticity(cell, false, params.curl, s.dt, 0.0, 0.0, 0.0, 0.0);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return fluid_divergence(cell, false, 0.8);
    if (s.pass == JFX_PASS_JACOBI)
        return fluid_jacobi(cell, false);
    if (s.pass == JFX_PASS_PROJECT)
        return fluid_project(cell, false, 0.0);
    if (s.pass == JFX_PASS_ADVECT_VEL)
        return fluid_advect_vel(cell, false, s.dt, velFade);
    return fluid_advect_rgb(cell, s.dt, params.decay);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
