// The generator library's own programs (generators/TitleGen.h, analysis/ReadGrid.h): the follow
// offset resample (Pachislop common.ts drawFollowed), the light glow (glowInto) and the analysis
// grid (effects/b/common.ts readGrid, generators/particles.ts readAlphaGrid), ported from
// Pachislop by SafEight, used with the author's permission.

#ifndef TITLEART_GEN_BASICS_GLSL
#define TITLEART_GEN_BASICS_GLSL

// Place: TA_TEX0 sampled where the drawing space (here the scratch's target px) puts this pixel;
// transparent outside it
vec4 gen_place() {
    vec2 q = gen_spacePos() / TA_SIZE;
    if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) return vec4(0.0);
    return texture(TA_TEX0, q);
}

// GlowAdd: TA_TEX0 x p6.x, alpha capped at 1 (drawn with LIGHT_PREMULTIPLIED)
vec4 gen_glowAdd() {
    vec4 c = texture(TA_TEX0, ta_uv()) * ta.p[6].x;
    return vec4(c.rgb, min(c.a, 1.0));
}

// Grid: the box means of TA_TEX0 over one cell. p6 = (x0, y0, cell, taps) in canvas px (the
// drawing space is document space: gen_toTarget maps into the SOURCE's target px), p7 = (w, h,
// packing: 0 luma, 1 colour), p8.xy = the source's size (TA_SIZE is the grid's). Memory row r holds
// grid row h-1-r, so the readback's rows reversed run bottom first. Taps sit on a regular lattice
// inside the cell; taps past the target count as empty.
vec4 gen_grid() {
    int i = int(gl_FragCoord.x);
    int j = int(ta.p[7].y) - 1 - int(gl_FragCoord.y);
    float cell = ta.p[6].z;
    int taps = int(ta.p[6].w);
    vec2 base = ta.p[6].xy + vec2(float(i), float(j)) * cell;
    float pitch = cell / float(taps);
    vec4 sum = vec4(0.0);
    float sumL = 0.0, sumS = 0.0;
    for (int y = 0; y < 16; y++) {
        if (y >= taps) break;
        for (int x = 0; x < 16; x++) {
            if (x >= taps) break;
            vec2 q = gen_toTarget(base + (vec2(float(x), float(y)) + 0.5) * pitch);
            if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, ta.p[8].xy))) continue;
            vec4 c = textureLod(TA_TEX0, q / ta.p[8].xy, 0.0);
            sum += c;
            sumL += ta_luma(c.rgb);
            vec3 s = ta_unpremul(c);
            float mx = max(s.r, max(s.g, s.b));
            sumS += mx > 1e-5 ? (mx - min(s.r, min(s.g, s.b))) / mx * c.a : 0.0;
        }
    }
    float n = float(taps * taps);
    vec4 mean = sum / n;
    if (ta.p[7].z > 0.5) {
        // colour: straight sRGB bytes and the mean alpha
        return vec4(ta_toSrgb(clamp(ta_unpremul(mean), 0.0, 1.0)), mean.a);
    }
    float lum = clamp(max(sumL / n, 0.0) / 4.0, 0.0, 1.0);
    vec2 l16 = ta_split16(lum * 65535.0);
    return vec4(mean.a, l16.x, l16.y, clamp(sumS / max(sum.a, 1e-5), 0.0, 1.0));
}

#endif // TITLEART_GEN_BASICS_GLSL
