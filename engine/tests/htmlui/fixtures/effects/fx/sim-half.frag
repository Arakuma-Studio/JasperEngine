#version 450
// @effect     sim-half
// @kind       paint
// @category   interactive
// @summary    EffectStatefulTest: half-float two- and one-channel states, ping-ponged and linearly sampled
// @cost       stateful
// @state      vel rg16f 2x1
// @state      prs r16f 2x1
// @init       seed-vel vel
// @init       seed-prs prs
// @step       move vel
// @step       press prs
// @param      scale number 2 0 4 -- Scales the pressure shown in G

// vel (rg16f, 2 x 1, linear by default): texel x holds (8 x + n, 1 + 2 n) after n steps; prs
// (r16f, 2 x 1): 8 x + 2 + n / 2. Every pass also writes 5 and 7 into the channels the format does
// not have: sampling must give 0 for the missing colour channels and 1 for alpha.
// The paint shows, each written so that the target holds it (sRGB of the linear value):
//   R = vel sampled between its two texels (linear: n + 4; nearest would give n or n + 8)
//   G = prs sampled between its two texels times scale (2: n + 12)
//   B = texel 1's vel.y (1 + 2 n) when the missing channels read (0, 1) and (0, 0, 1), else 0

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float scale;
} params;

JFX_STATE(vel)
JFX_STATE(prs)

vec4 jfx_paint(JfxContext c){
    vec4 v0 = vel_fetch(ivec2(0, 0));
    vec4 v1 = vel_fetch(ivec2(1, 0));
    vec4 p1 = prs_fetch(ivec2(1, 0));
    bool exact = v0.zw == vec2(0.0, 1.0) && p1.yzw == vec3(0.0, 0.0, 1.0);
    float r = vel_sample(vec2(0.5)).x;
    float g = prs_sample(vec2(0.5)).x * params.scale;
    float b = exact ? v1.y : 0.0;
    vec3 shown = clamp(vec3(r, g, b), 0.0, 255.0) / 255.0;
    return vec4(jfx_srgb_to_linear(shown), 1.0);
}

#ifdef JFX_STEP_FORM
vec4 jfx_step(JfxStep s){
    float x = float(s.cell.x);
    if (s.pass == JFX_PASS_SEED_VEL)
        return vec4(8.0 * x, 1.0, 5.0, 7.0);
    if (s.pass == JFX_PASS_SEED_PRS)
        return vec4(8.0 * x + 2.0, 5.0, 5.0, 7.0);
    if (s.pass == JFX_PASS_MOVE){
        vec4 v = vel_fetch(s.cell);
        return vec4(v.x + 1.0, v.y + 2.0, 5.0, 7.0);
    }
    // press
    return vec4(prs_fetch(s.cell).x + 0.5, 5.0, 5.0, 7.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
