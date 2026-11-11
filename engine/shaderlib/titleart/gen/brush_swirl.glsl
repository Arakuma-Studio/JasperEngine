// Brush Swirl (筆渦 / 墨), ported from Pachislop src/generators/rings/brushSwirl.ts by SafEight,
// used with the author's permission: the strokes' ribbon (MeshBrushSwirl) with bristle noise, a
// ragged rim and the dry-brush threshold rising toward the tail and the rims, and the splatter
// droplets (QuadsSplatter).
//
// Ribbon (analysis/BrushStroke.h brushRibbon): v_quad = (u along the stroke 0..1, px across from
// the centreline), v_extra = (arc length px, half width px, stroke seed, stroke width px);
// p6 = (linear colour rgb, dry 0-1), p7 = (reveal 0-1, 0, 0, 0): each stroke drawn up to u = reveal.
// Splatter: v_extra.x = the outline noise's offset; v_color the premultiplied colour.

#ifndef TITLEART_GEN_BRUSH_SWIRL_GLSL
#define TITLEART_GEN_BRUSH_SWIRL_GLSL

vec4 gen_brushSwirlRibbon() {
    float u = v_quad.x, across = v_quad.y;
    float len = v_extra.x, hw = max(v_extra.y, 1e-3), seed = v_extra.z, W = max(v_extra.w, 1.0);
    vec3 color = ta.p[6].rgb;
    float dry = ta.p[6].w;
    float reveal = ta.p[7].x;
    float sn = across / hw;     // -1 .. 1 across the ink
    float s = abs(sn);
    // hairs: clumps across the stroke (about W/10 + 3 per half width, converging in the tapers
    // like a real tip) stretched far along it, with finer single hairs inside them
    float f = W / 5.0 + 6.0;
    float b1 = vnoise(vec2(sn * f * 0.5 + seed * 31.0, len / (4.0 * W)));
    float b2 = vnoise(vec2(sn * f * 2.3 + seed * 17.0, len / (1.5 * W)));
    float b = 0.6 * b1 + 0.4 * b2;
    // ragged rim: the outermost hairs straggle
    float rim = 1.0 - 0.08 * vnoise(vec2(len / (0.3 * W), seed * 13.0)) - 0.08 * vnoise(vec2(len / (0.06 * W), seed * 7.0 + 3.0));
    float cov = clamp((rim * hw - abs(across)) / max(fwidth(across), 1e-4) + 0.5, 0.0, 1.0);
    // dry brush (かすれ): hairs run out of ink toward the tail and on the rims first, with dry
    // patches along the way where the brush skipped over the paper
    float skip = vnoise(vec2(len / (1.2 * W), seed * 5.0 + 11.0));
    float thr = dry * ((0.35 + 0.9 * pow(u, 1.2)) * (0.75 + 0.6 * s * s) + 0.5 * (skip - 0.5));
    // two value noises mix to a narrow band around 0.5: stretch it so the threshold above opens
    // real gaps (white streaks of paper) instead of hairlines
    float bs = clamp((b - 0.5) * 2.4 + 0.5, 0.0, 1.0);
    float aw = max(0.04, fwidth(bs));   // hair gaps stay anti-aliased when they get finer than a pixel
    float ink = dry > 0.0 ? smoothstep(thr - aw, thr + aw, bs) : 1.0;
    // the reveal cut, anti-aliased along u (fwidth taken before any branch)
    float du = max(fwidth(u), 1e-6);
    float shown = reveal >= 1.0 ? 1.0 : reveal <= 0.0 ? 0.0 : clamp((reveal - u) / du + 0.5, 0.0, 1.0);
    // sheen follows the hairs (bright / dark streaks in gold and red); the ink stays near solid
    float a = cov * ink * (0.94 + 0.06 * b1) * shown;
    return vec4(color * (0.45 + 1.1 * b2) * a, a);
}

vec4 gen_brushSwirlSplatter() {
    float d = length(v_quad) + 0.22 * (vnoise(v_quad * 2.5 + v_extra.x) - 0.5);
    float aa = 2.0 / max(min(v_sizePx.x, v_sizePx.y), 1.0);
    return v_color * clamp((0.8 - d) / aa + 0.5, 0.0, 1.0);
}

#endif // TITLEART_GEN_BRUSH_SWIRL_GLSL
