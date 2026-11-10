// Speed Lines (集中線 / 流線 / フラッシュ / ベタフラ), ported from Pachislop src/generators/lines/
// speedLines.ts (lineFrag, TABLE_GLSL, maskFrag, fillFrag) by SafEight, used with the author's
// permission. The layout is the CPU's (analysis/SpeedLineLayout); all positions are layer px.
//
// QuadsSpeedLines: one tapered wedge segment a quad. v_extra = (t at the quad's two ends: 0 =
// the base on the focus, 1 = the tip; half width and length in target px; a negative width is
// a flow line with no focus end). p6 = (taper fraction at the focus end, at the far end,
// opacity, 1 = colour by the gradient along the line); TA_TEX0 the gradient ramp.
//
// SpeedLinesBeta (ベタフラ, two fullscreen stages): p6 = (centre x, y, stage: 0 mask, 1 fill,
// fill mode: 0 solid, 1 gradient base -> frame edge, 2 gradient around); p7 = (linear colour,
// opacity). TA_TEX0 the spikes drawn white (mask stage) or the mask (fill stage), TA_TEX1 the
// gradient ramp, TA_RAW0 the 1,024-sample table (x = focus boundary plus teeth, y = frame exit,
// layer px from the centre; RGBA32F read with texelFetch).

#ifndef TITLEART_GEN_SPEED_LINES_GLSL
#define TITLEART_GEN_SPEED_LINES_GLSL

#include "titleart/gen/lines_common.glsl"

const float SL_TAU = 6.2831853;
const int SL_TAB = 1024;

#if TA_GEN_VERTICES == 1

vec4 gen_speedLinesQuad() {
    float tail = ta.p[6].x, head = ta.p[6].y, opacity = ta.p[6].z;
    float t = mix(v_extra.x, v_extra.y, 0.5 + 0.5 * v_quad.x);
    // a negative width: a flow line with no focus end, tapering like its far end on both ends
    float hw = abs(v_extra.z) * ln_taperAt(clamp(t, 0.0, 1.0), v_extra.z < 0.0 ? head : tail, head);
    float x = abs(v_quad.y) * 0.5 * v_sizePx.y;
    // sub-pixel widths keep a 1 px footprint and fade by their true width: hairlines, not dashes
    float cov = clamp(max(hw, 0.5) - x + 0.5, 0.0, 1.0) * min(1.0, 2.0 * hw);
    cov *= clamp(t * v_extra.w + 0.5, 0.0, 1.0) * clamp((1.0 - t) * v_extra.w + 0.5, 0.0, 1.0);
    vec4 c = v_color;
    if (ta.p[6].w > 0.5) {
        vec4 g = gen_ramp(TA_TEX0, clamp(t, 0.0, 1.0));
        c = vec4(g.rgb * g.a, g.a) * opacity;
    }
    return c * cov;
}

#else

// the focus boundary (x) and frame exit (y) at angle th, linear between the samples
vec2 sl_tableAt(float th) {
    float u = fract(th / SL_TAU) * float(SL_TAB);
    int i0 = int(floor(u)) % SL_TAB;
    int i1 = (i0 + 1) % SL_TAB;
    return mix(texelFetch(TA_RAW0, ivec2(i0, 0), 0).xy, texelFetch(TA_RAW0, ivec2(i1, 0), 0).xy, fract(u));
}

vec4 gen_speedLinesBeta() {
    vec2 d = gen_spacePos() - ta.p[6].xy;
    float th = atan(d.y, d.x);
    if (ta.p[6].z < 0.5) {
        // the mask: the spikes united with the (toothed) focus hole
        float hole = clamp((sl_tableAt(th).x - length(d)) * gen_spaceScale() + 0.5, 0.0, 1.0);
        return vec4(max(texture(TA_TEX0, ta_uv()).a, hole));
    }
    // the fill outside the mask
    float cov = 1.0 - texture(TA_TEX0, ta_uv()).a;
    int mode = int(ta.p[6].w + 0.5);
    vec4 c = vec4(ta.p[7].rgb, 1.0);
    if (mode == 1) {
        vec2 tb = sl_tableAt(th);
        c = gen_ramp(TA_TEX1, clamp((length(d) - tb.x) / max(tb.y - tb.x, 1.0), 0.0, 1.0));
    } else if (mode == 2) {
        c = gen_ramp(TA_TEX1, fract(th / SL_TAU));
    }
    return vec4(c.rgb * c.a, c.a) * (cov * ta.p[7].w);
}

#endif

#endif // TITLEART_GEN_SPEED_LINES_GLSL
