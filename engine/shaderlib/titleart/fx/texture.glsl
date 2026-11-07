// S83's texture effects (§6.6.18 Distress, §6.6.19 Drips, §6.6.23 Frost, §6.6.25 Chalk): original
// designs on the content's distance field (R27: they take their shape from the layer's own
// pixels, so an earlier glow never grows their band). Distress's dry brush is S80's かすれ code
// (titleart/render/fields.glsl TA_PROGRAM_ERODE, itself ported from Pachislop src/engine/layer2d/
// Title2DRenderer.ts by SafEight, used with the author's permission) on the content.
//
// Slots: TA_TEX0 the input L, TA_TEX1 the content (its alpha is the shape), TA_RAW0 the
// content's distance (+ outside, frame px).

#ifndef TITLEART_FX_TEXTURE_GLSL
#define TITLEART_FX_TEXTURE_GLSL

// a distance written by fx_writeDistance into the second raw slot
float tfx_readDistance1(ivec2 q) {
    vec4 t = texelFetch(TA_RAW1, clamp(q, ivec2(0), ivec2(TA_SIZE) - 1), 0);
#ifdef TA_DISTANCE_FIXED
    return (ta_bytes16(t.r, t.g) - 32768.0) / 64.0;
#else
    return t.r;
#endif
}

// the content's straight colour just inside its edge nearest to p (what spills, drips or dusts
// out of the shape takes the shape's colour there)
vec4 tfx_contentNear(vec2 p, float d, vec2 n) {
    vec2 q = p - n * (d + 1.5);
    return texture(TA_TEX1, q / TA_SIZE);
}

// ---- Distress ----

// the かすれ dry-brush keep-mask (fields.glsl TA_PROGRAM_ERODE) on the content: bristle lanes run
// along the drag direction and the dry ones lose ink over a random length measured along the drag
// from the silhouette
float tfx_inkAt(vec2 q) {
    return texture(TA_TEX1, q / TA_SIZE).a;
}
float tfx_exitDist(vec2 p, vec2 dir, float maxD) {
    float stepL = max(maxD / 48.0, 0.75);
    float t = 0.0;
    for (int i = 0; i < 96; i++) {
        float tn = t + stepL;
        if (tn > maxD) return maxD;
        if (textureLod(TA_TEX1, (p + dir * tn) / TA_SIZE, 0.0).a < 0.5) {
            float lo = t, hi = tn;
            for (int k = 0; k < 6; k++) {
                float m = 0.5 * (lo + hi);
                if (textureLod(TA_TEX1, (p + dir * m) / TA_SIZE, 0.0).a < 0.5) hi = m;
                else lo = m;
            }
            return 0.5 * (lo + hi);
        }
        t = tn;
    }
    return maxD;
}
vec2 tfx_sdGrad(vec2 q) {
    ivec2 i = ivec2(q);
    return vec2(tfx_distanceAt(i + ivec2(1, 0)) - tfx_distanceAt(i - ivec2(1, 0)), tfx_distanceAt(i + ivec2(0, 1)) - tfx_distanceAt(i - ivec2(0, 1)));
}
vec2 tfx_exitGrad(vec2 p, vec2 dir, float d, float maxD) {
    if (d >= maxD) return vec2(0.0);
    vec2 n = tfx_sdGrad(p + dir * d);
    float len = length(n);
    if (len < 1e-4) return vec2(0.0);
    float c = dot(dir, n);
    return -n / ((c < 0.0 ? -1.0 : 1.0) * max(abs(c), 0.05 * len));
}
// x lane dryness, y ink lost along the drag (px), z skip noise. lane, along: the brush's
// coordinates (lane px, length px, seeds)
vec3 tfx_brush(vec2 pl, vec2 dir, float amount, vec4 brush) {
    vec2 perp = vec2(-dir.y, dir.x);
    float lane = dot(pl, perp) / brush.y + brush.z;
    float along = dot(pl, dir) / brush.x + brush.w;
    float dry = 0.55 * vnoise(vec2(lane, along * 0.15)) + 0.3 * vnoise(vec2(lane * 0.21, 3.7 + along * 0.1)) + 0.15 * vnoise(vec2(lane * 2.7, along * 0.8));
    float x = clamp((amount - dry) / max(amount, 1e-3), 0.0, 1.0);
    float gap = brush.x * x * (0.45 + 0.8 * vnoise(vec2(lane * 0.37 + 11.0, along * 1.7)));
    return vec3(x, gap, vnoise(vec2(lane * 0.9 + 5.0, along * 4.0)));
}
float tfx_cut(float e, vec2 g) {
    float w = 0.7 * max(length(g), 1.0);
    return smoothstep(-w, w, e);
}
// p frame px, pl the same point in the brush's plane (canvas-anchored), edge band px
float tfx_dryBrushKeep(vec2 p, vec2 pl, vec2 dir, float amount, float edgePx, vec4 brush) {
    float depth = -tfx_distanceAt(ivec2(p));
    if (tfx_inkAt(p) <= 0.0 || depth > edgePx) return 1.0;
    vec3 b = tfx_brush(pl, dir, amount, brush);
    float x = b.x;
    if (x <= 0.0) return 1.0;
    vec3 bx = tfx_brush(pl + vec2(1.0, 0.0), dir, amount, brush) - b, by = tfx_brush(pl + vec2(0.0, 1.0), dir, amount, brush) - b;
    vec2 gX = vec2(bx.x, by.x), gGap = vec2(bx.y, by.y), gSkip = vec2(bx.z, by.z);
    float gap = b.y;
    float maxD = min(brush.x * 1.3, edgePx * 3.0);
    float dF = tfx_exitDist(p, dir, maxD);
    float dB = tfx_exitDist(p, -dir, maxD);
    float eat = max(tfx_cut(gap - dF, gGap - tfx_exitGrad(p, dir, dF, maxD)),
                    tfx_cut(gap * 0.35 - dB, gGap * 0.35 - tfx_exitGrad(p, -dir, dB, maxD)));
    eat = max(eat, tfx_cut(x * x * edgePx * 0.3 - depth, 2.0 * x * edgePx * 0.3 * gX + 0.5 * tfx_sdGrad(p)));
    float sw = max(0.125, 0.7 * length(gSkip));
    eat *= mix(1.0, smoothstep(0.325 - sw, 0.325 + sw, b.z), 0.45 * (1.0 - x));
    eat *= 1.0 - smoothstep(edgePx * 0.55, edgePx, depth);
    return 1.0 - eat;
}

// a keep threshold through a noise field n at p, antialiased by the field's own gradient
float tfx_threshold(float n, float nx, float ny, float t, float soft) {
    float g = length(vec2(nx - n, ny - n));
    float w = max(soft, 0.7 * g);
    return smoothstep(t - w, t + w, n);
}

// grunge, rough edges, scratches, speckle and spray keep-masks (1 = ink kept) at canvas point c
// (canvas px, y down), frame point p
float tfx_grunge(vec2 c, float scale, float seed) {
    vec2 q = c / scale + seed * 1.731;
    return 0.5 * tfx_fbm(q, 4) + 0.3 * vnoise(q * 0.37 + 9.1) + 0.2 * vnoise(q * 3.1 - 2.7);
}
float tfx_scratchLine(vec2 c, vec2 dir, float scale, float seed, float density) {
    vec2 perp = vec2(-dir.y, dir.x);
    float u = dot(c, dir), v = dot(c, perp);
    float lane = floor(v / scale);
    float v0 = (lane + 0.15 + 0.7 * hash12(vec2(lane, seed))) * scale;
    float seg = floor(u / (scale * 6.0) + hash12(vec2(lane, seed + 4.1)));
    float on = step(1.0 - density, hash12(vec2(lane * 3.1 + seg, seed + 8.3)));
    float halfW = (0.35 + 0.6 * hash12(vec2(seg, lane + seed))) / TA_PX;
    float t = fract(u / (scale * 6.0) + hash12(vec2(lane, seed + 4.1)));
    float ends = smoothstep(0.0, 0.15, t) * smoothstep(1.0, 0.75, t);
    return on * ends * clamp((halfW * 1.6 - abs(v - v0)) * TA_PX + 0.5, 0.0, 1.0);
}

// Distress: the layer worn away by grunge, dry brush, rough edges, scratches or speckle inside a
// band of `edge` px from the silhouette (0: the whole layer), what remains darkened by dirt; spray
// and speckle can also scatter ink outside the edge. TA_TEX0 L, TA_TEX1 the content, TA_RAW0 its
// distance. p2 = (pattern 0 grunge 1 dry brush 2 rough edges 3 scratches 4 speckle 5 spray,
// amount, edge px, scale canvas px), p3 = (direction (y down), contrast, wear), p4 = (wear colour
// linear, outside), p5 = (seed, canvas centre target px, brush lane px), p6 = (brush length px,
// brush seeds, edge band px for dry brush)
vec4 tfx_distress() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec4 C = texture(TA_TEX1, uv);
    int pattern = int(ta.p[2].x + 0.5);
    float amount = ta.p[2].y, edge = ta.p[2].z, scale = max(ta.p[2].w, 0.5);
    vec2 dir = ta.p[3].xy;
    float contrast = ta.p[3].z, wear = ta.p[3].w, outside = ta.p[4].w;
    float seed = ta.p[5].x;
    vec2 c = tfx_canvasPx(p, ta.p[5].yz);
    float d = tfx_distanceAt(ivec2(p));
    float depth = -d;
    // the band: full strength at the edge, none past `edge` px inside (the whole layer at 0)
    float band = edge > 0.0 ? 1.0 - smoothstep(0.0, edge, depth) : 1.0;
    float soft = 0.02 + 0.25 * (1.0 - contrast);
    float keep = 1.0;
    float spill = 0.0;
    float px1 = 1.0 / TA_PX;
    if (C.a > 0.0) {
        if (pattern == 0) {
            float n = tfx_grunge(c, scale, seed);
            keep = tfx_threshold(n, tfx_grunge(c + vec2(px1, 0.0), scale, seed), tfx_grunge(c + vec2(0.0, px1), scale, seed), amount * band, soft);
        } else if (pattern == 1) {
            vec4 brush = vec4(ta.p[6].x, ta.p[5].w, ta.p[6].yz);
            keep = tfx_dryBrushKeep(p, c * TA_PX, dir, amount, ta.p[6].w, brush);
        } else if (pattern == 2) {
            float n = tfx_fbm(c / scale + seed * 2.3, 4);
            // the edge eaten by up to the band (or a quarter of the noise scale) where the noise is high
            float bite = amount * (edge > 0.0 ? edge : 0.25 * scale * TA_PX) * n;
            keep = clamp(0.5 - (d + bite), 0.0, 1.0) / max(clamp(0.5 - d, 0.0, 1.0), 1e-3);
        } else if (pattern == 3) {
            keep = 1.0 - tfx_scratchLine(c, dir, scale, seed, amount) * band;
        } else if (pattern == 4) {
            float n = vnoise(c / (0.35 * scale) + seed * 5.1);
            float nx = vnoise((c + vec2(px1, 0.0)) / (0.35 * scale) + seed * 5.1), ny = vnoise((c + vec2(0.0, px1)) / (0.35 * scale) + seed * 5.1);
            keep = 1.0 - tfx_threshold(n, nx, ny, 1.0 - 0.6 * amount * band, soft);
        } else {
            float n = tfx_grunge(c, scale * 0.5, seed);
            keep = tfx_threshold(n, tfx_grunge(c + vec2(px1, 0.0), scale * 0.5, seed), tfx_grunge(c + vec2(0.0, px1), scale * 0.5, seed), 0.6 * amount * band, soft);
        }
    }
    // speckle and spray beyond the edge, within `edge` px (or `scale` when the band is the whole layer)
    float reachOut = edge > 0.0 ? edge : scale * TA_PX;
    if ((pattern == 4 || pattern == 5) && outside > 0.0 && d > 0.0 && d < reachOut) {
        vec3 w = tfx_worley(c / (pattern == 5 ? 0.6 * scale : 0.3 * scale), seed);
        float r = (pattern == 5 ? 0.18 : 0.12) * (0.4 + w.z);
        float dens = outside * (1.0 - d / reachOut);
        spill = step(1.0 - dens, w.z) * clamp((r - w.x) * (pattern == 5 ? 0.6 : 0.3) * scale * TA_PX + 0.5, 0.0, 1.0);
    }
    vec4 out_ = L;
    if (C.a > 0.0) {
        // dirt darkens what remains, more in the noise's low parts
        float dirt = wear * (1.0 - tfx_grunge(c * 1.7, scale, seed + 3.0));
        vec3 rgb = ta_unpremul(L);
        rgb = mix(rgb, ta.p[4].rgb, clamp(dirt, 0.0, 1.0));
        float k = mix(1.0, keep, C.a);
        out_ = vec4(rgb * L.a, L.a) * k;
    }
    if (spill > 0.0) {
        vec4 near = tfx_contentNear(p, d, tfx_distanceNormal(p));
        vec3 col = ta_unpremul(near);
        out_ = out_ + vec4(col, 1.0) * spill * (1.0 - out_.a);
    }
    return out_;
}

// ---- Drips ----

// One drip's origin: a point on an edge of the content that faces gravity, found by marching down
// candidate columns across the layer box (each column a seeded fraction of the box) and picking
// one of its gravity-facing edges, weighted toward the edges whose normal points with gravity.
// One output texel per drip, written as a jump-flood seed (x, y, valid). TA_TEX1 the content,
// TA_RAW0 its distance, TA_RAW1 its bounds. p2 = (gravity (y down), edge bias 0..1, seed),
// p3.x = the march step px
vec4 tfx_dripOrigins() {
    float i = floor(gl_FragCoord.x);
    vec2 g = ta.p[2].xy;
    vec2 u = vec2(-g.y, g.x);
    float bias = ta.p[2].z, seed = ta.p[2].w, stepPx = max(ta.p[3].x, 0.5);
    vec4 b = fx_layerBox();
    vec2 corners[4];
    corners[0] = b.xy;
    corners[1] = b.zy;
    corners[2] = b.xw;
    corners[3] = b.zw;
    float u0 = 1e9, u1 = -1e9, g0 = 1e9, g1 = -1e9;
    for (int k = 0; k < 4; k++) {
        u0 = min(u0, dot(corners[k], u));
        u1 = max(u1, dot(corners[k], u));
        g0 = min(g0, dot(corners[k], g));
        g1 = max(g1, dot(corners[k], g));
    }
    int steps = int(min(ceil((g1 - g0 + 2.0) / stepPx), 512.0));
    for (int cand = 0; cand < 8; cand++) {
        float uc = mix(u0, u1, hash12(vec2(i * 7.13 + float(cand) * 0.37, seed)));
        float pick = hash12(vec2(i * 3.71 + float(cand), seed + 19.3));
        // sweep 0 sums the edges' weights, sweep 1 walks to the picked one
        float total = 0.0, run = 0.0;
        for (int sweep = 0; sweep < 2; sweep++) {
            float prev = 0.0;
            for (int s = 0; s <= 512; s++) {
                if (s > steps) break;
                vec2 q = u * uc + g * (g0 - 1.0 + float(s) * stepPx);
                float a = textureLod(TA_TEX1, q / TA_SIZE, 0.0).a;
                if (s > 0 && prev >= 0.5 && a < 0.5) {
                    // the edge between the two samples, refined linearly
                    float t = (prev - 0.5) / max(prev - a, 1e-4);
                    vec2 e = q - g * stepPx * (1.0 - t);
                    vec2 n = tfx_distanceNormal(e);
                    float w = mix(1.0, pow(max(dot(n, g), 0.0), 2.0), bias) + 1e-3;
                    if (sweep == 0) {
                        total += w;
                    } else {
                        run += w;
                        if (run >= pick * total) return fx_writeSeed(vec3(e, 1.0));
                    }
                }
                prev = a;
            }
            if (total <= 0.0) break;
        }
    }
    return fx_writeSeed(vec3(0.0));
}

// a cone of radius r0 at a and r1 at b, rounded at both ends (Inigo Quilez's sdRoundCone,
// MIT, from his published distance-function article)
float tfx_roundCone(vec2 p, vec2 a, vec2 b, float r1, float r2) {
    vec2 ba = b - a;
    float l2 = dot(ba, ba);
    float rr = r1 - r2;
    float a2 = l2 - rr * rr;
    float il2 = 1.0 / max(l2, 1e-6);
    vec2 pa = p - a;
    float y = dot(pa, ba);
    float z = y - l2;
    vec2 xv = pa * l2 - ba * y;
    float x2 = dot(xv, xv);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = sign(rr) * rr * rr * x2;
    if (sign(z) * a2 * z2 > k) return sqrt(x2 + z2) * il2 - r2;
    if (sign(y) * a2 * y2 < k) return sqrt(x2 + y2) * il2 - r1;
    return (sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

// the drip i's shape from its origin: (radius at the top, radius of the head, length px)
vec3 tfx_dripShape(float i, float seed, vec4 shape) {
    float len = shape.x * (1.0 - shape.y * hash12(vec2(i * 1.17, seed + 2.9)));
    float w = shape.z * (1.0 - shape.w * hash12(vec2(i * 2.31, seed + 6.1)));
    return vec3(0.5 * w, len, w);
}

// The drips' distance field: the smallest distance to any drip (a cone from its origin along
// gravity, head rounded or pointed), written as a distance. TA_RAW0 the origins (seeds).
// p2 = (gravity (y down), count, seed), p3 = (length px, length random 0..1, width px, width
// random 0..1), p4.x = head (radius of the head / width; 0 a point)
vec4 tfx_dripField() {
    vec2 p = gl_FragCoord.xy;
    vec2 g = ta.p[2].xy;
    int count = int(ta.p[2].z + 0.5);
    float seed = ta.p[2].w;
    float head = ta.p[4].x;
    float best = 1e4;
    for (int i = 0; i < 256; i++) {
        if (i >= count) break;
        vec3 s = fx_readSeed(ivec2(i, 0));
        if (s.z < 0.5) continue;
        vec3 sh = tfx_dripShape(float(i), seed, ta.p[3]);
        if (sh.y < 0.5) continue;
        float r0 = sh.x;
        float r1 = max(head * sh.z * 0.5, 0.25);
        vec2 a = s.xy - g * r0;
        vec2 bEnd = s.xy + g * sh.y;
        best = min(best, tfx_roundCone(p, a, bEnd, r0, r1));
    }
    return fx_writeDistance(best);
}

// The drips merged with the shape (a smooth union of the two distances), coloured by the ink just
// inside the edge above (or a custom colour), shaded with a gloss streak along the drip and the
// style's look. TA_TEX0 L, TA_TEX1 the content, TA_RAW0 the content's distance, TA_RAW1 the
// drips' field. p2 = (merge px, colour mode 0 layer 1 custom, gloss, style 0 paint 1 slime
// 2 blood 3 icicles), p3 = (custom colour linear, width px), p4.xy = the light direction (y down)
vec4 tfx_dripCompose() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float dc = tfx_distanceAt(ivec2(p));
    float dd = tfx_readDistance1(ivec2(p));
    float k = max(ta.p[2].x, 1e-3);
    float h = max(k - abs(dc - dd), 0.0) / k;
    float d = min(dc, dd) - h * h * k * 0.25;
    float cov = clamp(0.5 - d, 0.0, 1.0);
    float shapeCov = clamp(0.5 - dc, 0.0, 1.0);
    float extra = max(cov - shapeCov, 0.0);
    if (extra <= 0.0) return L;
    int style = int(ta.p[2].w + 0.5);
    vec2 n = tfx_distanceNormal(p);
    vec3 col = ta.p[2].y > 0.5 ? ta.p[3].rgb : ta_unpremul(tfx_contentNear(p, dc, n));
    // the drip's bulge: 0 at its edge, 1 along its middle
    float halfW = max(0.5 * ta.p[3].w, 1.0);
    float bulge = clamp(-d / halfW, 0.0, 1.0);
    vec2 gx = vec2(tfx_readDistance1(ivec2(p) + ivec2(1, 0)) - tfx_readDistance1(ivec2(p) - ivec2(1, 0)),
                   tfx_readDistance1(ivec2(p) + ivec2(0, 1)) - tfx_readDistance1(ivec2(p) - ivec2(0, 1)));
    vec2 nd = length(gx) > 1e-4 ? normalize(gx) : vec2(0.0);
    vec3 N = normalize(vec3(nd * (1.0 - bulge), bulge + 0.25));
    vec3 Ld = normalize(vec3(ta.p[4].xy, 0.8));
    float diffuse = 0.75 + 0.25 * max(dot(N, Ld), 0.0);
    float spec = pow(max(dot(reflect(-Ld, N), vec3(0.0, 0.0, 1.0)), 0.0), 28.0) * ta.p[2].z;
    float alpha = 1.0;
    if (style == 1) {
        // slime: translucent, a light rim at its edge
        alpha = 0.82;
        col = col * 0.9 + vec3(0.25) * (1.0 - bulge) * (1.0 - bulge);
    } else if (style == 2) {
        // blood: darker toward the drip's head (farther from the shape)
        col *= mix(1.0, 0.55, clamp(dc / max(4.0 * halfW, 1.0), 0.0, 1.0));
    } else if (style == 3) {
        // icicles: translucent ice, the gloss facing up
        alpha = 0.9;
        spec = pow(max(dot(N, normalize(vec3(-ta.p[4].x * 0.3, -1.0, 0.6))), 0.0), 12.0) * ta.p[2].z;
    }
    vec3 rgb = col * diffuse + vec3(spec);
    vec4 drip = vec4(rgb, 1.0) * extra * alpha;
    return L + drip * (1.0 - L.a);
}

// ---- Frost ----

// Frost: ice crystals (Worley F1 / F2 cells) grow inward from the edge as the amount rises, rime
// crystals reach outward, facets lit by the light glint. TA_TEX0 L, TA_TEX1 the content, TA_RAW0
// its distance. p2 = (amount, reach px, outside px, crystal size canvas px), p3 = (frost colour
// linear, tint amount), p4 = (tint linear, sparkle), p5 = (seed, light direction (y down)),
// p6.xy = the canvas centre (target px)
vec4 tfx_frost() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float amount = ta.p[2].x, reach = ta.p[2].y, outsidePx = ta.p[2].z, size = max(ta.p[2].w, 0.5);
    float seed = ta.p[5].x;
    float d = tfx_distanceAt(ivec2(p));
    if (amount <= 0.0 || d > outsidePx + 1.0) return L;
    vec2 c = tfx_canvasPx(p, ta.p[6].xy);
    vec3 w = tfx_worley(c / size, seed);
    float facet = smoothstep(0.0, 0.18, w.y - w.x);
    // the frost front: each crystal reaches its own depth, deeper with the amount
    float front = amount * reach * (0.55 + 0.9 * w.z) - (1.0 - facet) * 0.15 * size * TA_PX;
    float inside = clamp(front + d + 0.5, 0.0, 1.0) * step(d, 0.5);
    float rime = 0.0;
    if (d > 0.0 && outsidePx > 0.0) {
        float out_ = amount * outsidePx * (0.3 + 0.9 * w.z);
        rime = clamp(out_ - d + 0.5, 0.0, 1.0) * facet * step(0.35, w.z);
    }
    vec3 ice = mix(ta.p[3].rgb, ta.p[4].rgb, ta.p[3].w);
    // a facet's normal from its cell, lit by the light
    vec2 fn = hash22(vec2(w.z * 71.3, seed)) * 2.0 - 1.0;
    vec3 N = normalize(vec3(fn * 0.8, 1.0));
    vec3 Ld = normalize(vec3(ta.p[5].yz, 0.7));
    float glint = pow(max(dot(reflect(-Ld, N), vec3(0.0, 0.0, 1.0)), 0.0), 20.0) * ta.p[4].w * facet;
    vec3 frostRgb = ice * (0.75 + 0.25 * facet) + vec3(glint);
    float cov = max(inside * (0.65 + 0.35 * facet), 0.0);
    vec4 outC = L;
    if (L.a > 0.0 && cov > 0.0) {
        vec3 base = ta_unpremul(L);
        outC = vec4(mix(base, frostRgb, cov) * L.a, L.a);
    }
    if (rime > 0.0) outC = outC + vec4(frostRgb, 1.0) * rime * (1.0 - outC.a);
    return outC;
}

// ---- Chalk ----

// Chalk: the shape drawn in chalk strokes: anisotropic noise along the stroke angle times a paper
// grain sets the coverage, the edge is broken by noise, dust specks fall outside. TA_TEX0 L,
// TA_TEX1 the content, TA_RAW0 its distance. p2 = (grain canvas px, coverage, stroke length canvas
// px, edge rough), p3 = (stroke direction (y down), edge width px, dust), p4 = (chalk colour linear,
// colour mode 0 layer 1 custom), p5 = (dust reach px, seed, canvas centre target px)
vec4 tfx_chalk() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float grainS = max(ta.p[2].x, 0.25), coverage = ta.p[2].y, strokeLen = max(ta.p[2].z, 0.5), rough = ta.p[2].w;
    vec2 dir = ta.p[3].xy;
    float edgeW = ta.p[3].z, dust = ta.p[3].w, dustReach = ta.p[5].x, seed = ta.p[5].y;
    vec2 c = tfx_canvasPx(p, ta.p[5].zw);
    float d = tfx_distanceAt(ivec2(p));
    // the edge, broken by noise
    float er = rough * edgeW * (tfx_fbm(c / max(0.8 * grainS, 0.25) + seed, 3) - 0.5) * 2.0;
    float shape = clamp(0.5 - (d + max(er, -0.5 * edgeW)), 0.0, 1.0);
    vec2 perp = vec2(-dir.y, dir.x);
    vec2 sc = vec2(dot(c, dir) / strokeLen, dot(c, perp) / (0.6 * grainS));
    float strokes = vnoise(sc + seed * 3.3) * 0.6 + vnoise(sc * vec2(2.1, 1.7) + 7.7) * 0.4;
    float paper = vnoise(c / grainS + seed * 9.1) * 0.6 + vnoise(c / (0.45 * grainS) - 3.1) * 0.4;
    float n = 0.55 * strokes + 0.45 * paper;
    float thr = 1.0 - coverage;
    float k = smoothstep(thr - 0.08, thr + 0.08, n);
    vec3 chalkRgb = ta.p[4].w > 0.5 ? ta.p[4].rgb : mix(ta_unpremul(texture(TA_TEX1, uv)), vec3(1.0), 0.12);
    vec4 C = texture(TA_TEX1, uv);
    vec4 outC = L;
    if (C.a > 0.0 || shape > 0.0) {
        // chalk replaces the layer's own pixels; what earlier effects drew around them stays
        vec4 rest = max(L - C, vec4(0.0));
        if (ta.p[4].w < 0.5 && C.a <= 0.0) chalkRgb = mix(ta_unpremul(tfx_contentNear(p, d, tfx_distanceNormal(p))), vec3(1.0), 0.12);
        float a = shape * k;
        outC = vec4(chalkRgb, 1.0) * a + rest * (1.0 - a);
    }
    if (d > 0.0 && dust > 0.0 && d < dustReach) {
        float speck = step(1.0 - dust * 0.35 * (1.0 - d / dustReach), hash12(floor(c / max(0.35 * grainS, 0.25)) + seed));
        vec3 dc = ta.p[4].w > 0.5 ? ta.p[4].rgb : ta_unpremul(tfx_contentNear(p, d, tfx_distanceNormal(p)));
        outC = outC + vec4(dc, 1.0) * speck * 0.6 * (1.0 - outC.a);
    }
    return outC;
}

#endif // TITLEART_FX_TEXTURE_GLSL
