#version 450
// @effect     time-trail
// @kind       filter
// @category   stylize
// @summary    Whatever moves sheds a smooth, decaying long-exposure ghost trail
// @source     shaders.com TimeTrail
// @cost       stateful
// @state      history rgba16f 640x640 linear
// @state      display rgba16f 640x640 linear
// @state      prev-live rgba16f 640x640 nearest
// @step       decay display
// @step       stamp history
// @step       keep prev-live
// @suggest-for echo-trail motion-trail ghost-trail
// @param      trail-source enum motion motion|alpha -- What sheds the trail: what changes between frames, or the content's coverage
// @param      motion-threshold number 0.06 0.01 0.5 -- How much a pixel must change to shed a trail (motion)
// @param      trail-length time 0.6s 0.05s 4s -- How long the trail lingers (the fade's time constant)
// @param      diffusion number 1 0 1 -- How much the trail smears and softens as it fades
// @param      drift-x number 0 -1 1 -- Sideways drift of the trail per second
// @param      drift-y number 0 -1 1 -- Vertical drift of the trail per second
// @param      zoom number 1 0.9 1.1 -- Per-frame zoom of the trail around the centre (above 1 an outward tunnel)
// @param      trail-opacity number 1 0 1 -- Strength of the trail
// @param      tint-mode enum none none|color|rainbow -- Colour the trail: one tint, or a hue that cycles as it ages
// @param      tint color #33aaff -- The tint of tint-mode color
// @param      trail-blend enum normal normal|add|screen|multiply|overlay|soft-light|hard-light|darken|lighten|color-dodge|color-burn|linear-burn|difference|exclusion|hue|saturation|color|luminosity -- How the trail combines with the frame
// @param      speed number 1 0 4 -- Simulation speed (0 freezes the trail)

// Ported from shaders.com TimeTrail (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/TimeTrail/index.ts, gpu/scaffolds/gridKernels.ts buildFeedbackTrailStep,
// std/sim/feedback.ts). A 640 x 640 history of the content (premultiplied, as the layer):
// every step it is advected (zoom about the centre, drift), softened by a four-tap tent, faded by
// exp(-dt / trail-length), optionally hue-cycled, then the live content is stamped on top (gated
// by the frame difference in motion mode; screen/add accumulate light). Upstream's one kernel
// writes the history, the display copy (motion: the trail before the stamp) and the previous
// frame: three passes here. The composite uses the kit's blend modes over the live content.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_sim_blend.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 tint;
    float trailSource;
    float motionThreshold;
    float trailLength;
    float diffusion;
    float driftX;
    float driftY;
    float zoom;
    float trailOpacity;
    float tintMode;
    float trailBlend;
    float speed;
} params;

JFX_STATE(history)
JFX_STATE(display)
JFX_STATE(prev_live)

const float STATE_RES = 640.0;
const float DIFFUSE_HALF_UV = 2.5 * 0.5 / 640.0;
const float DRIFT_SCALE = 0.5;
const float RAINBOW_RATE = 0.35;

// trailBlend -> the kit's mode (fam_blend_mode's numbering)
int trail_mode(){
    return int(params.trailBlend + 0.5);
}

vec4 jfx_filter(JfxContext c){
    bool motion = params.trailSource < 0.5;
    vec4 live = jfx_src(c.px);
    vec4 trail = (motion ? display_sample(c.uv) : history_sample(c.uv));
    if (params.tintMode > 0.5 && params.tintMode < 1.5){
        // colour mode: the trail recoloured toward the tint by its luminance (premultiplied)
        float a = max(trail.a, 0.0001);
        float lum = dot(trail.rgb / a, vec3(0.299, 0.587, 0.114));
        trail = vec4(params.tint.rgb * (lum * 1.5) * trail.a, trail.a);
    }
    int mode = trail_mode();
    if (motion){
        // the ghosts over the (usually opaque) live frame, trail-opacity the blend's opacity
        vec4 blended = fam_blend_mode(live, jfx_unpremultiply(trail), params.trailOpacity, mode);
        return jfx_unpremultiply(blended);
    }
    // alpha: the crisp live frame over the trail
    vec4 trailStraight = jfx_unpremultiply(trail * params.trailOpacity);
    return jfx_unpremultiply(fam_blend_mode(trailStraight, live, 1.0, mode));
}

#ifdef JFX_STEP_FORM
vec2 trail_feedback_uv(vec2 uv, float dt){
    // zoom is a per-frame divide: raised to a dt-scaled power (60 fps the reference cadence)
    float zoom = max(pow(params.zoom, dt * 60.0), 0.01);
    return vec2((uv.x - 0.5) / zoom + 0.5 - params.driftX * dt * DRIFT_SCALE, (uv.y - 0.5) / zoom + 0.5 - params.driftY * dt * DRIFT_SCALE);
}

vec4 trail_decayed(vec2 b, float dt){
    float persistence = exp(-dt / max(params.trailLength, 0.001));
    float h = params.diffusion * DIFFUSE_HALF_UV;
    vec4 sum = history_sample(vec2(b.x - h, b.y - h)) + history_sample(vec2(b.x + h, b.y - h)) + history_sample(vec2(b.x - h, b.y + h)) +
               history_sample(vec2(b.x + h, b.y + h));
    vec4 dec = sum * (persistence * 0.25);
    if (params.tintMode > 1.5){
        // rainbow: the hue rotated a touch each step (compounds into a spectrum down the trail)
        float a = max(dec.w, 0.0001);
        vec3 hsl = jfx_rgb_to_hsl(dec.xyz / a);
        vec3 rotated = jfx_hsl_to_rgb(vec3(fract(hsl.x + RAINBOW_RATE * dt), hsl.y, hsl.z));
        dec = vec4(rotated * dec.w, dec.w);
    }
    return dec;
}

vec4 jfx_step(JfxStep s){
    float dt = min(s.dt, 0.05) * params.speed;
    vec2 uv = (vec2(s.cell) + 0.5) / STATE_RES;
    // the live content at this texel (premultiplied)
    vec4 live = jfx_premultiply(jfx_src(uv * s.c.size));
    if (s.pass == JFX_PASS_KEEP)
        return live;
    if (s.pass == JFX_PASS_DECAY)
        return trail_decayed(trail_feedback_uv(uv, dt), dt);
    // the stamp over the decayed trail (the display image holds it)
    vec4 trail = display_fetch(s.cell);
    float m = 1.0;
    if (params.trailSource < 0.5){
        // frameDiffMask
        vec4 prev = prev_live_fetch(s.cell);
        vec3 dRgb = abs(live.xyz - prev.xyz);
        float diff = dot(dRgb, vec3(0.299, 0.587, 0.114)) + abs(live.w - prev.w);
        m = smoothstep(params.motionThreshold, params.motionThreshold * 2.0 + 0.001, diff);
    }
    // the injected splats stamp the live content under them
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        float d = length(uv * s.c.size - sp.pos);
        m = max(m, sp.strength * (1.0 - smoothstep(sp.radius * 0.5, sp.radius, d)));
    }
    vec4 st = vec4(live.xyz * m, live.w * m);
    float outA = st.w + trail.w * (1.0 - st.w);
    vec3 rgbNormal = st.xyz + trail.xyz * (1.0 - st.w);
    vec3 rgbScreen = st.xyz + trail.xyz - st.xyz * trail.xyz;
    vec3 rgbAdd = st.xyz + trail.xyz;
    // the stamp's light: screen and add accumulate inside the history, every other mode is source-over
    int mode = trail_mode();
    vec3 rgb = mode == 2 ? rgbScreen : (mode == 1 ? rgbAdd : rgbNormal);
    return vec4(rgb, outA);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
