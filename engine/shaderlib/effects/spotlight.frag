#version 450
// @effect     spotlight
// @kind       paint
// @category   interactive
// @summary    A soft light that follows the pointer over the element
// @source     original
// @cost       low
// @pointer    position
// @suggest-for cursor-light flashlight
// @param      color color #ffffff -- Light colour
// @param      radius length 160px 8px 2000px -- Radius of the light
// @param      softness number 0.6 0 1 -- Share of the radius that fades out
// @param      intensity number 0.35 0 1 -- Strength while the pointer is over the element
// @param      idle-position position "50% 50%" -- Where the light rests while the pointer is away
// @param      idle-intensity number 0 0 1 -- Strength while the pointer is away (eased by presence)

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec2 idlePosition;
    float radius;
    float softness;
    float intensity;
    float idleIntensity;
} params;

vec4 jfx_paint(JfxContext c){
    // the light eases between its resting place and the smoothed pointer as the pointer arrives
    vec2 center = mix(params.idlePosition * c.size, c.pointerSmooth, c.pointerPresence);
    float strength = mix(params.idleIntensity, params.intensity, c.pointerPresence);
    float radius = max(params.radius, 1e-3);
    float inner = radius * (1.0 - clamp(params.softness, 0.0, 1.0));
    float d = length(c.px - center);
    // a smooth falloff from the inner radius to the edge (no visible ring where it starts)
    float t = clamp((d - inner) / max(radius - inner, 1e-3), 0.0, 1.0);
    float light = 1.0 - t * t * (3.0 - 2.0 * t);
    return vec4(params.color.rgb, params.color.a * light * strength);
}

#include "effects/lib/main_paint.glsl"
