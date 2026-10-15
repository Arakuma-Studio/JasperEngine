#version 450
// @effect     thin-film
// @kind       paint
// @category   light
// @summary    An iridescent soap-bubble rim along the element's edges
// @source     shaders.com ThinFilm (adapted: rim along the element edges)
// @cost       medium
// @time       speed
// @suggest-for soap-bubble iridescence
// @param      intensity number 1 0 4 -- Brightness of the rim (above 1 pushes it past white)
// @param      rim-width number 1 0 3 -- How far in from the edges the iridescence reaches
// @param      edge-softness number 0.3 0 1 -- Softness of the rim's outer edge
// @param      thickness number 0.5 0 1 -- Film thickness: more spectral bands across the rim
// @param      dispersion number 0.5 0 1 -- How much the colour differs between the lit and the shaded sides
// @param      saturation number 1 0 1 -- Spectral saturation (0 a white rim)
// @param      hue-shift number 0 0 1 -- Turns the spectrum (1 is a full turn)
// @param      light-angle angle 300deg 0deg 360deg -- Which side of the rim runs warm and which cool
// @param      mode enum rainbow rainbow|custom -- The full spectrum, or a cycle through the three colours
// @param      color-a color #2b6fff -- First colour of the custom cycle
// @param      color-b color #ffffff -- Second colour of the custom cycle
// @param      color-c color #ff7a21 -- Third colour of the custom cycle
// @param      color-space enum oklch linear|oklch|oklab|hsl|hsv|lch -- Colour space of the custom cycle
// @param      speed number 0.1 -1 1 -- Turns per second of the colours around the rim

// Ported from shaders.com ThinFilm (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (kit/effects/thinFilm.ts: thinFilmShade on the 2D path, thinFilmRainbow, thinFilmCustomParams,
// thinFilmCompose). Adapted: the rim follows the element's rounded box (jfx_box_sdf); the
// element's shorter side spans the source's default shape (0.7 units), so the rim's width and
// softness scale with the element. The custom cycle mixes only the segment its gates select
// (the same colour; the source mixes all three and selects one).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sdf.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    float intensity;
    float rimWidth;
    float edgeSoftness;
    float thickness;
    float dispersion;
    float saturation;
    float hueShift;
    float lightAngle;
    float mode;
    float colorSpace;
    float speed;
} params;

vec4 jfx_paint(JfxContext c){
    float unitPx = max(min(c.size.x, c.size.y), 1.0) / 0.7;
    float sdf = jfx_box_sdf(c.px, c.size, c.radii) / unitPx;
    vec2 nrm = jfx_box_normal(c.px, c.size, c.radii);

    // thinFilmShade (2D): the inside mask, the rim band, the directional phase turning over time
    float sharp = max(params.edgeSoftness * 0.5, 0.001);
    float rb1 = clamp(-sdf / sharp * 32.0, 0.0, 1.0);
    float rimWidth = max(params.rimWidth * 0.1, 0.001);
    float depth = clamp(-sdf / rimWidth, 0.0, 1.0);
    float rim = (1.0 - depth) * (1.0 - depth) * rb1;
    float lx = cos(params.lightAngle);
    float ly = sin(params.lightAngle);
    float baseCos = nrm.x * lx + nrm.y * ly;
    float baseSin = nrm.y * lx - nrm.x * ly;
    float theta = c.anim.x * JFX_TAU;
    float rotatedDot = baseCos * cos(theta) + baseSin * sin(theta);
    float phase = params.hueShift + params.dispersion * rotatedDot + params.thickness * depth * 2.0;

    vec3 baseColor;
    if (params.mode < 0.5){
        // thinFilmRainbow: the cosine palette with the truncated thirds
        baseColor = vec3(cos(phase * JFX_TAU) * 0.5 + 0.5, cos((phase + 0.3333) * JFX_TAU) * 0.5 + 0.5,
                         cos((phase + 0.6667) * JFX_TAU) * 0.5 + 0.5);
    }else{
        // thinFilmCustomParams: A -> B -> C -> A, the segment's two colours mixed in the space
        float t3 = fract(phase) * 3.0;
        float si = fract(t3);
        float segment = floor(t3);
        vec4 from = segment < 0.5 ? params.colorA : (segment < 1.5 ? params.colorB : params.colorC);
        vec4 to = segment < 0.5 ? params.colorB : (segment < 1.5 ? params.colorC : params.colorA);
        baseColor = jfx_mix_colors(from, to, si, params.colorSpace).rgb;
    }
    // thinFilmCompose: white to the spectrum by saturation, the rim times the intensity
    vec3 col = mix(vec3(1.0), baseColor, params.saturation);
    return vec4(col * rim * params.intensity, clamp(rim, 0.0, 1.0));
}

#include "effects/lib/main_paint.glsl"
