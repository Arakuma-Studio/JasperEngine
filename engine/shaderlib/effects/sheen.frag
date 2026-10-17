#version 450
// @effect     sheen
// @kind       paint
// @category   light
// @summary    A band of light sweeping across the element now and then, like a glint on glass
// @source     original
// @cost       low
// @suggest-for shine glint-sweep
// @param      color color #ffffff -- Colour of the band
// @param      angle angle 20deg -90deg 90deg -- Lean of the band from vertical (it sweeps across it, left to right)
// @param      width number 0.25 0.02 1 -- Width of the band as a share of the sweep
// @param      softness number 0.6 0 1 -- Share of the band's half-width that fades out (0 crisp)
// @param      intensity number 0.6 0 1 -- Strength of the band
// @param      duration time 0.8s 0.05s 10s -- How long a sweep takes to cross the element
// @param      period time 3s 0.1s 60s -- Time from one sweep to the next
// @param      delay time 0s 0s 60s -- Wait before the first period starts

// One sweep per period, centred in it (the band crosses in the middle of each period, with equal
// rests before and after), from just outside the left edge to just outside the right edge. The
// band runs along the direction `angle` leans from vertical; the sweep's 0..1 spans the element's
// extent across the band, so it starts and ends off the element at every angle.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/fam_light_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float angle;
    float width;
    float softness;
    float intensity;
    float duration;
    float period;
    float delay;
} params;

vec4 jfx_paint(JfxContext c){
    float duration = max(params.duration, 1e-3);
    float period = max(params.period, 1e-3);
    float t = c.time - params.delay;
    float phase = t - period * floor(t / period);
    float progress = (phase - max(period - duration, 0.0) * 0.5) / duration;
    if (t < 0.0 || progress < 0.0 || progress > 1.0)
        return vec4(params.color.rgb, 0.0);
    // the position across the band, 0..1 over the element
    vec2 dir = vec2(cos(params.angle), sin(params.angle));
    float extent = max(c.size.x * abs(dir.x) + c.size.y * abs(dir.y), 1e-3);
    float u = dot(c.px - c.size * 0.5, dir) / extent + 0.5;
    float halfWidth = max(params.width, 1e-3) * 0.5;
    float center = mix(-halfWidth, 1.0 + halfWidth, progress);
    float d = abs(u - center) / halfWidth;
    float profile = 1.0 - fam_light_smoothstep(1.0 - clamp(params.softness, 0.0, 1.0), 1.0, d);
    return vec4(params.color.rgb, params.color.a * params.intensity * profile);
}

#include "effects/lib/main_paint.glsl"
