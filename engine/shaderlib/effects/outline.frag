#version 450
// @effect     outline
// @kind       filter
// @category   stylize
// @summary    A coloured stroke around the content's shape (text, icons, images with alpha)
// @source     original
// @cost       medium
// @margin     width+2px
// @suggest-for stroke text-stroke
// @param      color color #ffffff -- Colour of the stroke
// @param      width length 2px 0px 16px -- Width of the stroke
// @param      softness number 0 0 1 -- Softness of the stroke's edge (0 crisp)
// @param      position enum outside outside|inside|center -- Where the stroke sits: around, inside or across the content's edge

// The stroke is a morphological band of the content's alpha: the alpha dilated (outside) or
// eroded (inside) by `width` (half of it each way for center), the largest or smallest alpha of
// 16 directions on two radii (the width and half of it) plus this pixel. Softness blends the
// extreme toward the taps' mean, which feathers the band's far edge. Outside, the dilated shape
// goes under the content; inside, the stroke colours the content where the eroded shape is not.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float width;
    float softness;
    float position;
} params;

// the alpha dilated (grow) or eroded (shrink) by radius: (extreme, mean) of the taps
vec2 outline_morph(vec2 px, float radius, float a0, bool grow){
    float extreme = a0;
    float sum = a0;
    for (int ring = 0; ring < 2; ring++){
        float r = radius * (ring == 0 ? 1.0 : 0.5);
        for (int i = 0; i < 16; i++){
            float ang = JFX_TAU * (float(i) + 0.5 * float(ring)) / 16.0;
            float a = jfx_src(px + vec2(cos(ang), sin(ang)) * r).a;
            extreme = grow ? max(extreme, a) : min(extreme, a);
            sum += a;
        }
    }
    return vec2(extreme, sum / 33.0);
}

vec4 jfx_filter(JfxContext c){
    vec4 src = jfx_decode(jfx_src_premul_here());
    float a0 = src.a;
    float width = max(params.width, 0.0);
    if (width <= 0.0)
        return src;
    float soft = clamp(params.softness, 0.0, 1.0);
    float outer = 0.0;    // the stroke's coverage around the content (under it): the dilated alpha
    float inner = 0.0;    // the stroke's share of the content's colour (over it): 1 - the eroded alpha
    float radius = params.position > 1.5 ? width * 0.5 : width;
    if (params.position < 0.5 || params.position > 1.5){
        vec2 grown = outline_morph(c.px, radius, a0, true);
        outer = mix(grown.x, clamp(grown.y * 2.0, 0.0, 1.0), soft);
    }
    if (params.position > 0.5){
        vec2 shrunk = outline_morph(c.px, radius, a0, false);
        inner = 1.0 - mix(shrunk.x, clamp(shrunk.y * 2.0 - 1.0, 0.0, 1.0), soft);
    }
    // the inner stroke over the content's colour, then the content over the outer stroke
    vec3 content = mix(src.rgb, params.color.rgb, params.color.a * inner);
    float outA = params.color.a * outer;
    vec3 premul = content * a0 + params.color.rgb * outA * (1.0 - a0);
    float alpha = a0 + outA * (1.0 - a0);
    return vec4(alpha > 1e-5 ? premul / alpha : vec3(0.0), alpha);
}

#include "effects/lib/main_filter.glsl"
