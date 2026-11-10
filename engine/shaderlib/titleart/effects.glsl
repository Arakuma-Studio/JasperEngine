// Title Art's effect and generator kernels (docs/specs/S80-title-art-core.md §6.5): the shared
// helpers of the effect library (copy, separable Gaussian, jump-flood distance field, bounds
// reduction) and the ten core effects and two generators. Ported from Pachislop
// src/effects/lib.ts, src/effects/a/glow.ts, shadow.ts, finish.ts (keyline), stylize.ts
// (gradientOverlay), blur.ts, color.ts, src/engine/layer2d/shaders.ts (FILL_GLSL) and
// src/generators/pattern.ts by SafEight, used with the author's permission.
//
// Each fx_* function is one program's body (titleart.frag's main for TA_PROGRAM_*); its values
// are the p[2..] of u_fs_titleart in the layout effects/TitleFxPrograms.cpp packs. Directions
// arrive as frame-pixel vectors (y down) and colours as the caller says: "linear" (the
// effect blends in linear light, Pachislop's THREE.Color uniforms) or "sRGB" (it blends
// sRGB-encoded values like Photoshop). Gradient ramps are RGBA8 lookups of sRGB-encoded stops
// (TitleArtCaps' GradientLut): ta_toLinear decodes where Pachislop sampled a decoding texture.

#ifndef TITLEART_EFFECTS_GLSL
#define TITLEART_EFFECTS_GLSL

// ---- the distance-field and bounds encodings ----

// a jump-flood seed: xy the sub-pixel edge point (target px), z 1 when there is one
vec3 fx_readSeed(ivec2 q) {
    vec4 t = texelFetch(TA_RAW0, q, 0);
#ifdef TA_SEEDS_FIXED
    // RGBA8: (x + 1) and (y + 1) as 12.4 fixed point in two bytes each, 0xFFFF = no seed
    float x = ta_bytes16(t.r, t.g), y = ta_bytes16(t.b, t.a);
    if (x > 65534.5) return vec3(0.0);
    return vec3(x / 16.0 - 1.0, y / 16.0 - 1.0, 1.0);
#else
    return vec3(t.xy, t.z > 0.5 ? 1.0 : 0.0);
#endif
}

vec4 fx_writeSeed(vec3 s) {
#ifdef TA_SEEDS_FIXED
    if (s.z < 0.5) return vec4(1.0);
    return vec4(ta_split16(min((s.x + 1.0) * 16.0, 65534.0)), ta_split16(min((s.y + 1.0) * 16.0, 65534.0)));
#else
    return s.z > 0.5 ? vec4(s.xy, 1.0, 0.0) : vec4(-1.0);
#endif
}

// signed distance to the alpha edge in target px, + outside
float fx_readDistance(ivec2 q) {
    vec4 t = texelFetch(TA_RAW0, q, 0);
#ifdef TA_DISTANCE_FIXED
    // RGBA8: 1/64 px steps around 32768 in RG, +-512 px
    return (ta_bytes16(t.r, t.g) - 32768.0) / 64.0;
#else
    return t.r;
#endif
}

vec4 fx_writeDistance(float d) {
#ifdef TA_DISTANCE_FIXED
    return vec4(ta_split16(clamp(d * 64.0 + 32768.0, 0.0, 65535.0)), 0.0, 1.0);
#else
    return vec4(d, 0.0, 0.0, 1.0);
#endif
}

// a bounds texel: two values (min x, min y in the even column; max x, max y in the odd one), in
// texel indices of the reduced image
vec2 fx_decodeBoundsPair(vec4 t) {
#ifdef TA_SEEDS_FIXED
    return vec2(ta_bytes16(t.r, t.g), ta_bytes16(t.b, t.a));
#else
    return t.xy;
#endif
}

vec4 fx_writeBoundsPair(vec2 v) {
#ifdef TA_SEEDS_FIXED
    return vec4(ta_split16(v.x), ta_split16(v.y));
#else
    return vec4(v, 0.0, 0.0);
#endif
}

// The layer's box (lib.ts layerBox): the texel centres of its pixels with alpha > 0.004, in
// target px (x0, y0, x1, y1); the whole target when it has none. TA_RAW1 holds the 2 x 1 result
// of the bounds reduction.
vec4 fx_layerBox() {
    vec2 lo = fx_decodeBoundsPair(texelFetch(TA_RAW1, ivec2(0, 0), 0));
    vec2 hi = fx_decodeBoundsPair(texelFetch(TA_RAW1, ivec2(1, 0), 0));
    if (hi.x < lo.x || hi.y < lo.y) return vec4(0.0, 0.0, TA_SIZE);
    return vec4(lo + 0.5, hi + 0.5);
}

// position 0..1 along the unit vector `dir` across the layer's box (lib.ts boxPos)
float fx_boxPos(vec2 dir) {
    vec4 b = fx_layerBox();
    vec2 c = 0.5 * (b.xy + b.zw), hs = 0.5 * (b.zw - b.xy);
    float ext = max(abs(dir.x) * hs.x + abs(dir.y) * hs.y, 1.0);
    return 0.5 + dot(gl_FragCoord.xy - c, dir) / (2.0 * ext);
}

// ---- shared passes ----

// p2.x: opacity (a scale of the premultiplied colour)
vec4 fx_copy() {
    return texture(TA_TEX0, ta_uv()) * ta.p[2].x;
}

// One 1-D Gaussian pass (lib.ts blur, blur.ts gauss): p2 = (uv step per tap, sigma in level
// texels, repeat edges), p3.x = the largest tap offset. Without Repeat Edge Pixels taps outside
// the whole render read transparent but still count in the weight sum, so content touching the
// frame edge fades (After Effects); with it they clamp to the edge texel.
vec4 fx_gauss() {
    vec2 uv = ta_uv();
    vec2 stepUv = ta.p[2].xy;
    float sigma = ta.p[2].z;
    bool repeatEdges = ta.p[2].w > 0.5;
    if (sigma < 0.01) return texture(TA_TEX0, uv);
    int r = min(int(ceil(sigma * 3.0)), int(ta.p[3].x));
    float k = -0.5 / (sigma * sigma);
    vec4 frame = ta_frameUv();
    vec4 acc = vec4(0.0);
    float ws = 0.0;
    for (int i = -r; i <= r; i++) {
        vec2 q = uv + stepUv * float(i);
        float w = exp(k * float(i * i));
        ws += w;
        if (!repeatEdges && (q.x < frame.x || q.y < frame.y || q.x > frame.z || q.y > frame.w)) continue;
        acc += texture(TA_TEX0, repeatEdges ? clamp(q, frame.xy, frame.zw) : q) * w;
    }
    return acc / ws;
}

// Jump-flood seeds (lib.ts seedFrag): EDGE POINTS estimated from the anti-aliased alpha (the
// pixel centre moved along the alpha gradient to where it crosses the threshold), so distances
// are smooth along slanted edges. p2.x: threshold. Every coordinate is in target px of one frame
// space, so the gradient's orientation needs no convention.
float fx_alphaAt(vec2 q) {
    ivec2 i = ivec2(clamp(q, vec2(0.5), TA_SIZE - 0.5));
    return texelFetch(TA_TEX0, i, 0).a;
}

vec4 fx_seed() {
    float threshold = ta.p[2].x;
    vec2 p = gl_FragCoord.xy;
    float a = fx_alphaAt(p);
    bool inside = a >= threshold;
    bool edge = (fx_alphaAt(p + vec2(1.0, 0.0)) >= threshold) != inside || (fx_alphaAt(p - vec2(1.0, 0.0)) >= threshold) != inside ||
                (fx_alphaAt(p + vec2(0.0, 1.0)) >= threshold) != inside || (fx_alphaAt(p - vec2(0.0, 1.0)) >= threshold) != inside;
    if (!edge) return fx_writeSeed(vec3(0.0));
    float tl = fx_alphaAt(p + vec2(-1.0, 1.0)), t = fx_alphaAt(p + vec2(0.0, 1.0)), tr = fx_alphaAt(p + vec2(1.0, 1.0));
    float l = fx_alphaAt(p + vec2(-1.0, 0.0)), r = fx_alphaAt(p + vec2(1.0, 0.0));
    float bl = fx_alphaAt(p + vec2(-1.0, -1.0)), b = fx_alphaAt(p + vec2(0.0, -1.0)), br = fx_alphaAt(p + vec2(1.0, -1.0));
    vec2 g = vec2(tr + 2.0 * r + br - tl - 2.0 * l - bl, tl + 2.0 * t + tr - bl - 2.0 * b - br);
    if (dot(g, g) < 1e-8) return fx_writeSeed(vec3(p, 1.0));
    vec2 n = normalize(g);
    // the alpha slope per px along n, the steeper one-sided difference: a hard step reads as 1
    float slope = max(max(fx_alphaAt(p + n) - a, a - fx_alphaAt(p - n)), 1e-3);
    return fx_writeSeed(vec3(p - n * clamp((a - threshold) / slope, -1.0, 1.0), 1.0));
}

// one flood step (lib.ts jfaFrag): p2.x the step in px
vec4 fx_jfa() {
    float stepPx = ta.p[2].x;
    vec2 p = gl_FragCoord.xy;
    vec3 best = vec3(0.0);
    float dBest = 1e20;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 q = p + vec2(x, y) * stepPx;
            if (q.x < 0.0 || q.y < 0.0 || q.x >= TA_SIZE.x || q.y >= TA_SIZE.y) continue;
            vec3 s = fx_readSeed(ivec2(q));
            if (s.z > 0.5) {
                float d = dot(s.xy - p, s.xy - p);
                if (d < dBest) {
                    dBest = d;
                    best = s;
                }
            }
        }
    }
    return fx_writeSeed(best);
}

// the signed distance (lib.ts sdfFrag): TA_RAW0 the seeds, TA_TEX0 the source, p2.x threshold.
// A pixel no seed reached (beyond the flood's reach) reads 1e4 (clamped by the RGBA8 encoding):
// consumers only need "at least their reach" there.
vec4 fx_distance() {
    vec2 p = gl_FragCoord.xy;
    vec3 s = fx_readSeed(ivec2(p));
    float a = texelFetch(TA_TEX0, ivec2(p), 0).a;
    float d = s.z > 0.5 ? length(s.xy - p) : 1e4;
    return fx_writeDistance(a >= ta.p[2].x ? -d : d);
}

// One level of the bounds reduction (lib.ts boundsFrag): each output pair of texels (even column
// the minimum, odd the maximum) reduces a 4 x 4 block of the level below. p2.x: 1 on the first
// level, which reads TA_TEX0's alpha (> 0.004) instead of TA_RAW0's pairs.
vec4 fx_bounds() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    int side = o.x - (o.x / 2) * 2;
    ivec2 block = ivec2(o.x / 2, o.y) * 4;
    bool first = ta.p[2].x > 0.5;
    ivec2 size = first ? textureSize(TA_TEX0, 0) : textureSize(TA_RAW0, 0) / ivec2(2, 1);
    vec2 m = side == 0 ? vec2(65535.0) : vec2(0.0);
    bool any = false;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            ivec2 q = block + ivec2(x, y);
            if (q.x >= size.x || q.y >= size.y) continue;
            if (first) {
                if (texelFetch(TA_TEX0, q, 0).a > 0.004) {
                    m = side == 0 ? min(m, vec2(q)) : max(m, vec2(q));
                    any = true;
                }
            } else {
                vec2 lo = fx_decodeBoundsPair(texelFetch(TA_RAW0, ivec2(q.x * 2, q.y), 0));
                vec2 hi = fx_decodeBoundsPair(texelFetch(TA_RAW0, ivec2(q.x * 2 + 1, q.y), 0));
                if (hi.x >= lo.x && hi.y >= lo.y) {
                    m = side == 0 ? min(m, lo) : max(m, hi);
                    any = true;
                }
            }
        }
    }
    if (!any) m = side == 0 ? vec2(65535.0) : vec2(0.0);
    return fx_writeBoundsPair(m);
}

// ---- the effects ----

// Outer Glow (glow.ts outerGlow). TA_TEX0 the input L, TA_TEX1 the big blur, TA_TEX2 the small
// blur, TA_TEX3 the ramp, TA_RAW0 the distance, TA_RAW1 the bounds.
// p2 = (colour linear, intensity), p3 = (spread, opacity, on face, size px),
// p4 = (fill 0 solid 1 gradient 2 layer 3 distance, mode 0 behind 1 add, precise), p5.xy dir
vec4 fx_outerGlow() {
    vec2 uv = ta_uv();
    vec3 color = ta.p[2].rgb;
    float intensity = ta.p[2].w;
    float spread = ta.p[3].x, opacity = ta.p[3].y, onFace = ta.p[3].z, size = ta.p[3].w;
    int fill = int(ta.p[4].x + 0.5);
    int mode = int(ta.p[4].y + 0.5);
    bool isPrecise = ta.p[4].z > 0.5;
    vec4 L = texture(TA_TEX0, uv);
    vec4 B = texture(TA_TEX1, uv);
    float d = isPrecise || fill == 3 ? fx_readDistance(ivec2(gl_FragCoord.xy)) : 0.0;
    float a;
    if (isPrecise) {
        a = (1.0 - smoothstep(min(spread, 0.999), 1.0, max(d, 0.0) / size)) * clamp(size - d + 0.5, 0.0, 1.0);
    } else {
        // From Layer: the layer's own light, one Gaussian; else a tighter core is mixed in
        a = fill == 2 ? B.a : 0.6 * B.a + 0.4 * texture(TA_TEX2, uv).a;
        a = clamp(a / max(1.0 - spread, 0.02), 0.0, 1.0);
    }
    vec3 col = fill == 2 ? B.rgb / max(B.a, 1e-6)
             : fill == 1 ? ta_toLinear(texture(TA_TEX3, vec2(clamp(fx_boxPos(ta.p[5].xy), 0.0, 1.0), 0.5)).rgb)
             : fill == 3 ? ta_toLinear(texture(TA_TEX3, vec2(clamp(max(d, 0.0) / size, 0.0, 1.0), 0.5)).rgb)
             : color;
    float g = clamp(a * intensity, 0.0, 1.0) * opacity;
    if (mode == 0) return L + vec4(col * g, g) * (1.0 - L.a);
    // additive light (may exceed alpha: emissive), over the face by On Face
    return vec4(L.rgb + col * a * intensity * opacity * (1.0 - (1.0 - onFace) * L.a), L.a + g * (1.0 - L.a));
}

// Inner Glow (glow.ts innerGlow). TA_TEX0 L, TA_TEX1 the blur, TA_TEX3 the ramp, TA_RAW0 the
// distance. p2 = (colour linear, intensity), p3 = (opacity, choke, size px, mode 0 normal
// 1 screen 2 add), p4 = (centre, precise, distance fill)
vec4 fx_innerGlow() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    if (L.a < 1e-5) return vec4(0.0);
    vec3 color = ta.p[2].rgb;
    float intensity = ta.p[2].w;
    float opacity = ta.p[3].x, choke = ta.p[3].y, size = ta.p[3].z;
    int mode = int(ta.p[3].w + 0.5);
    bool center = ta.p[4].x > 0.5, isPrecise = ta.p[4].y > 0.5, distanceFill = ta.p[4].z > 0.5;
    float d = isPrecise || distanceFill ? fx_readDistance(ivec2(gl_FragCoord.xy)) : 0.0;
    float g;
    if (isPrecise) {
        // edge: solid in to Choke of Size, then a smooth falloff to 0 at Size; centre: the reverse
        float s = smoothstep(min(choke, 0.999), 1.0, max(-d, 0.0) / size);
        g = center ? s : (1.0 - s) * clamp(size + d + 0.5, 0.0, 1.0);
    } else {
        float b = texture(TA_TEX1, uv).a;
        g = (center ? (b - 0.5) * 2.0 : 2.0 * (1.0 - b)) / max(1.0 - choke, 0.02);
    }
    g = clamp(g * intensity, 0.0, 1.0) * opacity;
    vec3 col = distanceFill ? ta_toLinear(texture(TA_TEX3, vec2(clamp(max(-d, 0.0) / size, 0.0, 1.0), 0.5)).rgb) : color;
    vec3 c = ta_unpremul(L);
    vec3 r;
    if (mode == 0) r = mix(c, col, g);
    else if (mode == 1) r = c + col * g - c * col * g;
    else r = c + col * g;
    return vec4(r * L.a, L.a);
}

// Drop Shadow (shadow.ts dropShadow). TA_TEX0 L, TA_TEX1 the blurred input.
// p2 = (offset px, spread, opacity), p3.rgb the colour (linear)
vec4 fx_dropShadow() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float a = texture(TA_TEX1, uv - ta.p[2].xy / TA_SIZE).a;
    a = clamp(a / max(1.0 - ta.p[2].z, 0.02), 0.0, 1.0) * ta.p[2].w;
    return L + vec4(ta.p[3].rgb * a, a) * (1.0 - L.a);
}

// Inner Shadow (shadow.ts innerShadow): the layer's own alpha, shifted and blurred, casts the
// shadow inward where it does not cover the layer; outside the whole render counts as empty.
// TA_TEX0 L, TA_TEX1 the blurred input. p2 = (offset px, choke, opacity), p3.rgb the colour
vec4 fx_innerShadow() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec2 q = uv - ta.p[2].xy / TA_SIZE;
    vec4 frame = ta_frameUv();
    float inside = all(greaterThanEqual(q, frame.xy)) && all(lessThanEqual(q, frame.zw)) ? texture(TA_TEX1, q).a : 0.0;
    float s = clamp((1.0 - inside) / max(1.0 - ta.p[2].z, 0.02), 0.0, 1.0) * ta.p[2].w;
    // source-atop: the shadow over the layer, clipped to its alpha
    return vec4(L.rgb * (1.0 - s) + ta.p[3].rgb * s * L.a, L.a);
}

// Outline (finish.ts keyline): a line traced from the alpha, behind the layer just outside the
// silhouette. TA_TEX0 the input L, TA_TEX1 the content (or ink), TA_TEX2 the input without its
// light, TA_RAW0 the distance of what is followed. p2 = (colour linear, width px),
// p3 = (softness, opacity, follow layer, lit)
vec4 fx_outline() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float d = fx_readDistance(ivec2(gl_FragCoord.xy));
    float width = ta.p[2].w;
    float soft = ta.p[3].x;
    bool followLayer = ta.p[3].z > 0.5, lit = ta.p[3].w > 0.5;
    float k = 1.0 - smoothstep(width * (1.0 - soft) - 0.5, width + 0.5, d);
    float a = k * ta.p[3].y;
    vec4 K = vec4(ta.p[2].rgb * a, a);
    if (lit) {
        // light in the stack: the line goes with the ink S (the stack without the light), then
        // the light L - S is added over it as light adds (alpha by its share)
        vec4 S = texture(TA_TEX2, uv);
        vec4 C = followLayer ? texture(TA_TEX1, uv) : S;
        vec4 base = C + K * (1.0 - C.a) + (S - C) * (1.0 - K.a);
        float la = S.a < 0.999 ? clamp((L.a - S.a) / (1.0 - S.a), 0.0, 1.0) : 0.0;
        return max(vec4(base.rgb + L.rgb - S.rgb, base.a + (1.0 - base.a) * la), 0.0);
    }
    if (!followLayer) return L + K * (1.0 - L.a);
    // Follow Layer: behind the layer's own pixels C but over what the stack drew around them
    vec4 C = texture(TA_TEX1, uv);
    return max(C + K * (1.0 - C.a) + (L - C) * (1.0 - K.a), 0.0);
}

// Color Overlay: the colour blended over the layer's own colour (sRGB-encoded, Photoshop's),
// alpha kept. TA_TEX0 L. p2 = (colour sRGB, opacity), p3.x the blend mode
vec4 fx_colorOverlay() {
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return vec4(0.0);
    vec3 b = ta_toSrgb(ta_unpremul(L));
    vec3 r = ta_toLinear(mix(b, ta_blend(int(ta.p[3].x + 0.5), b, ta.p[2].rgb), ta.p[2].w));
    return vec4(r * L.a, L.a);
}

// Gradient Overlay (stylize.ts gradientOverlay). TA_TEX0 L, TA_TEX1 the ramp, TA_RAW1 the bounds.
// p2 = (dir, scale, offset), p3 = (amount, length px, position -1..1, mode: the 18 blend modes,
// 18 light (add), 19 shade (multiply)), p4 = (wrap 0 clamp 1 repeat 2 mirror, style 0 linear
// 1 radial 2 sweep)
vec4 fx_gradientOverlay() {
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return vec4(0.0);
    vec2 dir = ta.p[2].xy;
    float scale = ta.p[2].z, offset = ta.p[2].w;
    float amount = ta.p[3].x, len = ta.p[3].y, pos = ta.p[3].z;
    int mode = int(ta.p[3].w + 0.5);
    int wrap = int(ta.p[4].x + 0.5), style = int(ta.p[4].y + 0.5);
    vec4 bb = fx_layerBox();
    vec2 c = 0.5 * (bb.xy + bb.zw), hs = 0.5 * (bb.zw - bb.xy);
    vec2 p = gl_FragCoord.xy - c;
    float ext = max(abs(dir.x) * hs.x + abs(dir.y) * hs.y, 1.0);
    float x;
    if (style == 1) {
        // radial: 0 at the box centre to 1 at its corners (elliptical, the box's aspect)
        x = length(p / max(hs, vec2(1.0))) * 0.70710678 / max(scale, 0.01) + offset;
    } else if (style == 2) {
        // sweep: a len-px gradient centred at pos (-1..1) of the way from just off one side to the other
        x = 0.5 + (dot(p, dir) - pos * (ext + 0.5 * len)) / len;
    } else {
        x = dot(p, dir) / (2.0 * ext) / max(scale, 0.01) + 0.5 + offset;
    }
    if (wrap == 1) x = fract(x);
    else if (wrap == 2) x = 1.0 - abs(mod(x, 2.0) - 1.0);
    vec3 g = texture(TA_TEX1, vec2(clamp(x, 0.0, 1.0), 0.5)).rgb;
    vec3 gl = ta_toLinear(g);
    vec3 r;
    if (mode == 18) {
        // light: linear-light addition, unclamped (emissive)
        r = ta_unpremul(L) + gl * amount;
    } else if (mode == 19) {
        // shade: linear-light multiply (light through a filter)
        r = ta_unpremul(L) * mix(vec3(1.0), gl, amount);
    } else {
        vec3 b = ta_toSrgb(ta_unpremul(L));
        r = ta_toLinear(mix(b, ta_blend(mode, b, g), amount));
    }
    return vec4(r * L.a, L.a);
}

#ifdef TITLEART_PATTERNS_GLSL
// Pattern Overlay: the procedural marks (and their secondary colour) blended over the layer's
// colour, alpha kept. TA_TEX0 L, TA_TEX1 the custom pattern's image. p2 = motif = M frag + t:
// M row-major (a, b, c, d), p3 = (t, kind, seed), p4 = (colour sRGB, opacity),
// p5 = (colour 2 sRGB, blend mode)
vec4 fx_patternOverlay() {
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return vec4(0.0);
    vec2 f = gl_FragCoord.xy;
    vec4 m = ta.p[2];
    vec2 motif = vec2(dot(m.xy, f), dot(m.zw, f)) + ta.p[3].xy;
    vec4 pat = patternSampleD(motif, vec2(m.x, m.z), vec2(m.y, m.w), int(ta.p[3].z + 0.5), ta.p[3].w);
    int mode = int(ta.p[5].w + 0.5);
    float opacity = ta.p[4].w;
    vec3 b = ta_toSrgb(ta_unpremul(L));
    vec3 r = mix(b, ta_blend(mode, b, ta.p[4].rgb), pat.x * opacity);
    r = mix(r, ta_blend(mode, r, ta.p[5].rgb), pat.y * opacity);
    return vec4(ta_toLinear(r) * L.a, L.a);
}
#endif

#ifdef TITLEART_ADJUST_GLSL
// Adjust: one adjustment (adjust.glsl) on the layer's pixels. TA_TEX0 L, TA_TEX1 the lookup.
// p2.x the op, p3..p5 its values
vec4 fx_adjust() {
    vec4 s = texture(TA_TEX0, ta_uv());
    if (s.a <= 0.0) return vec4(0.0);
    vec3 c = ta_toSrgb(clamp(s.rgb / s.a, 0.0, 1.0));
    c = clamp(ta_adjust(int(ta.p[2].x + 0.5), c, ta.p[3], ta.p[4], ta.p[5]), 0.0, 1.0);
    return vec4(ta_toLinear(c) * s.a, s.a);
}
#endif

// ---- the generators ----

// Fill (layer2d/shaders.ts FILL_GLSL over the canvas, Pachislop's background layer): p is canvas
// px from its bottom-left, +y up (Pachislop's comp space): p = M frag + t. TA_TEX0 the ramp.
// p2 = M row-major, p3 = (t, kind 0 solid 1 linear 2 radial 3 angle 4 reflected 5 diamond
// 6 shape burst, scale), p4 = (colour sRGB, offset), p5 = (dir y-up, canvas size)
vec4 fx_fill() {
    vec2 f = gl_FragCoord.xy;
    vec4 m = ta.p[2];
    vec2 p = vec2(dot(m.xy, f), dot(m.zw, f)) + ta.p[3].xy;
    int kind = int(ta.p[3].z + 0.5);
    vec2 size = ta.p[5].zw;
    vec4 col;
    if (kind == 0) {
        col = vec4(ta.p[4].rgb, 1.0);
    } else {
        vec2 c = 0.5 * size;
        vec2 hs = 0.5 * size;
        vec2 v = p - c;
        vec2 dir = ta.p[5].xy;
        float s = max(ta.p[3].w, 1e-3);
        float ext = max(abs(dir.x) * hs.x + abs(dir.y) * hs.y, 1e-3);
        float t;
        if (kind == 1) {
            t = dot(v, dir) / (2.0 * ext * s) + 0.5;
        } else if (kind == 2) {
            t = length(v) / (max(length(hs), 1e-3) * s);
        } else if (kind == 3) {
            // clockwise on screen (y up: a negative cross) from the gradient direction, per turn
            float a = atan(-(dir.x * v.y - dir.y * v.x), dot(dir, v));
            t = (a < 0.0 ? a + 6.2831853 : a) / (6.2831853 * s);
        } else if (kind == 4) {
            t = abs(dot(v, dir)) / (ext * s);
        } else if (kind == 5) {
            t = (abs(v.x) + abs(v.y)) / (max(hs.x + hs.y, 1e-3) * s);
        } else {
            // the canvas rectangle's shape burst: distance to the nearest edge over the half short side
            vec2 e = min(p, size - p);
            t = min(e.x, e.y) / max(0.5 * min(size.x, size.y), 1e-3) / s;
        }
        col = texture(TA_TEX0, ta_lutUv(t - ta.p[4].w));
    }
    return vec4(ta_toLinear(col.rgb) * col.a, col.a);
}

#ifdef TITLEART_PATTERNS_GLSL
// Pattern Fill (generators/pattern.ts): the patterns over the whole frame on a background,
// optionally on a floor plane receding to a horizon. lp = layer px (+y up) = M frag + t.
// p2 = M row-major, p3 = (t, kind, seed), p4 = (colour linear, background opacity),
// p5 = (colour 2 linear, motif px), p6 = (background linear, focal px), p7 = (sin tilt, cos tilt,
// rotation cos, rotation sin)
vec4 fx_patternFill() {
    vec2 f = gl_FragCoord.xy;
    vec4 m = ta.p[2];
    vec2 lp = vec2(dot(m.xy, f), dot(m.zw, f)) + ta.p[3].xy;
    float focal = ta.p[6].w;
    vec2 tilt = ta.p[7].xy;
    // a floor tilted back about the layer's x axis, seen from `focal` in front of the origin:
    // screen y = f v cos t / (f + v sin t), screen x = f u / (f + v sin t), inverted here
    float den = focal * tilt.y - lp.y * tilt.x;
    float v = lp.y * focal / max(den, 1e-3 * focal);
    vec2 plane = vec2(lp.x * (focal + v * tilt.x) / focal, v);
    // motif units: one per Motif Size, turned clockwise on screen by Rotation
    float k = 1.0 / max(ta.p[5].w, 1e-3);
    vec2 cs = ta.p[7].zw * k;
    vec2 q = vec2(cs.x * plane.x - cs.y * plane.y, cs.y * plane.x + cs.x * plane.y);
    vec4 pat = patternSampleD(q, dFdx(q), dFdy(q), int(ta.p[3].z + 0.5), ta.p[3].w);
    // past ~4 px per motif the marks only shimmer: they fade toward the horizon (resolution
    // independent: motif units per layer px)
    float perLayerPx = length(fwidth(q)) / max(length(m.xz), 1e-6);
    float a = den > 0.0 ? 1.0 - smoothstep(0.25, 0.7, perLayerPx) : 0.0;
    float bgA = ta.p[4].w;
    vec4 c = vec4(ta.p[6].rgb * bgA, bgA);
    c = mix(c, vec4(ta.p[4].rgb, 1.0), pat.x);
    c = mix(c, vec4(ta.p[5].rgb, 1.0), pat.y);
    return c * a;
}
#endif

#endif // TITLEART_EFFECTS_GLSL
