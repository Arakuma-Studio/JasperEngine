#version 450
// @effect     sim-probe
// @kind       paint
// @category   interactive
// @summary    EffectStatefulTest: a simulation that counts its steps and the splats it took
// @cost       stateful
// @pointer    position
// @state      count rgba32f 1x1 nearest
// @state      field rgba16f box*0.25 max 64 linear resample
// @init       seed count
// @step       tick count
// @step       grow field
// @reseed     seed
// @param      seed number 0 0 100 -- Restarts the count when it changes
// @param      gain number 1 0 4 -- Scales the splats' strength

// count (one texel): x the steps counted on the GPU since the reset, y the simulation time of the
// last step, z the splat strength taken (times gain), w the pointer's x at the last step (CSS px).
// The paint shows R = x / 255, G = z / 255, B = the field's age (fract(x / 60)), each written so
// that the target holds it (sRGB of the linear value).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float seed;
    float gain;
} params;

JFX_STATE(count)
JFX_STATE(field)

vec4 jfx_paint(JfxContext c){
    vec4 n = count_fetch(ivec2(0));
    vec4 f = field_sample(c.uv);
    vec3 v = vec3(min(n.x, 255.0) / 255.0, min(n.z, 255.0) / 255.0, f.z);
    return vec4(jfx_srgb_to_linear(v), 1.0);
}

#ifdef JFX_STEP_FORM
vec4 jfx_step(JfxStep s){
    if (s.pass == JFX_PASS_SEED)
        return vec4(0.0, s.time, 0.0, 0.0);
    if (s.pass == JFX_PASS_TICK){
        vec4 n = count_fetch(ivec2(0));
        float taken = 0.0;
        for (int i = 0; i < 8; i++){
            if (i >= s.splatCount)
                break;
            taken += jfx_splat(i).strength * params.gain;
        }
        return vec4(n.x + 1.0, s.time, n.z + taken, s.pointerKnown ? s.pointer.x : -1.0);
    }
    // grow: the age of the count, the texel's uv
    vec4 n = count_fetch(ivec2(0));
    return vec4(s.uv, fract(n.x / 60.0), 1.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
