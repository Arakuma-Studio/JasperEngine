// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/layer2d/shaders.ts (SEED_FRAG, JFA_FRAG, SDF_FRAG,
// HEIGHT_FRAG, COVER_*_FRAG, REDUCE_MAX_FRAG, BLUR_R_FRAG, GROW_FRAG, ERODE_FRAG,
// MASK_KEEP_FRAG, NEAREST_GLYPH_FRAG) and src/effects/lib.ts (the alpha seeds of sdf()) by
// SafEight, used with the author's permission.

// The 2D title's mask-derived fields (§6.7 passes 3-9) and the effects' distance field. The
// element frames are y-up (row 0 the frame's lowest row, common.glsl), so gl_FragCoord is the
// frame pixel of Pachislop's formulas; the alpha distance of node targets runs in their own
// rows (distances do not care). Scalar outputs go through taEncodeScalar (TA_OUT_ENC), scalar
// inputs through taDecodeScalar (TA_RAW_ENC), seeds through taEncodeSeed / taDecodeSeed.

#if TA_PROGRAM == TA_PROGRAM_SEED
// coverage (TA_TEX0) -> seeds at sub-pixel edge points: the pixel centre moved along the
// coverage gradient by its offset from 0.5, so distances are smooth along slanted edges
float taCov(ivec2 q) {
    ivec2 mx = textureSize(TA_TEX0, 0) - 1;
    return texelFetch(TA_TEX0, clamp(q, ivec2(0), mx), 0).r;
}
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2 p = gl_FragCoord.xy;
    float a = taCov(ip);
    // edge pixels: partial coverage, or a hard (non-anti-aliased) step to a 4-neighbour
    bool edge = a > 0.003 && a < 0.997;
    if (!edge) {
        float o4 = 1.0 - a;
        edge = abs(taCov(ip + ivec2(1, 0)) - o4) < 0.003 || abs(taCov(ip - ivec2(1, 0)) - o4) < 0.003
            || abs(taCov(ip + ivec2(0, 1)) - o4) < 0.003 || abs(taCov(ip - ivec2(0, 1)) - o4) < 0.003;
    }
    if (!edge) { frag_color = taEncodeSeed(vec2(0.0), false, TA_OUT_ENC); return; }
    // Sobel gradient of the coverage: the normal pointing inside (y-up frame)
    float tl = taCov(ip + ivec2(-1, 1)), t = taCov(ip + ivec2(0, 1)), tr = taCov(ip + ivec2(1, 1));
    float l = taCov(ip + ivec2(-1, 0)), r = taCov(ip + ivec2(1, 0));
    float bl = taCov(ip + ivec2(-1, -1)), b = taCov(ip + ivec2(0, -1)), br = taCov(ip + ivec2(1, -1));
    vec2 g = vec2(tr + 2.0 * r + br - tl - 2.0 * l - bl, tl + 2.0 * t + tr - bl - 2.0 * b - br);
    vec2 n = dot(g, g) > 1e-8 ? normalize(g) : vec2(0.0);
    frag_color = taEncodeSeed(p - n * (a - 0.5), true, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_SEED_ALPHA
// a layer target's alpha (TA_TEX0) -> seeds at its threshold crossing (p[2].x the threshold)
float taAlphaAt(vec2 q) {
    vec2 res = vec2(textureSize(TA_TEX0, 0));
    return texture(TA_TEX0, clamp(q, vec2(0.5), res - 0.5) / res).a;
}
void main() {
    float threshold = ta.p[2].x;
    vec2 p = gl_FragCoord.xy;
    float a = taAlphaAt(p);
    // edge pixels: a 4-neighbour on the other side of the threshold; faint alpha far from the
    // shape (glow halos) seeds nothing and keeps its true distance
    bool inside = a >= threshold;
    bool edge = (taAlphaAt(p + vec2(1.0, 0.0)) >= threshold) != inside || (taAlphaAt(p - vec2(1.0, 0.0)) >= threshold) != inside
             || (taAlphaAt(p + vec2(0.0, 1.0)) >= threshold) != inside || (taAlphaAt(p - vec2(0.0, 1.0)) >= threshold) != inside;
    if (!edge) { frag_color = taEncodeSeed(vec2(0.0), false, TA_OUT_ENC); return; }
    float tl = taAlphaAt(p + vec2(-1.0, 1.0)), t = taAlphaAt(p + vec2(0.0, 1.0)), tr = taAlphaAt(p + vec2(1.0, 1.0));
    float l = taAlphaAt(p + vec2(-1.0, 0.0)), r = taAlphaAt(p + vec2(1.0, 0.0));
    float bl = taAlphaAt(p + vec2(-1.0, -1.0)), b = taAlphaAt(p + vec2(0.0, -1.0)), br = taAlphaAt(p + vec2(1.0, -1.0));
    vec2 g = vec2(tr + 2.0 * r + br - tl - 2.0 * l - bl, tl + 2.0 * t + tr - bl - 2.0 * b - br);
    if (dot(g, g) < 1e-8) { frag_color = taEncodeSeed(p, true, TA_OUT_ENC); return; }
    vec2 n = normalize(g);
    // the alpha's slope per px along n, the steeper one-sided difference (a hard step reads 1)
    float slope = max(max(taAlphaAt(p + n) - a, a - taAlphaAt(p - n)), 1e-3);
    frag_color = taEncodeSeed(p - n * clamp((a - threshold) / slope, -1.0, 1.0), true, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_FLOOD
// one jump-flood step of p[2].x px over the seeds (TA_RAW0)
void main() {
    float stepPx = ta.p[2].x;
    vec2 p = gl_FragCoord.xy;
    ivec2 res = textureSize(TA_RAW0, 0);
    vec3 best = vec3(0.0);
    float dBest = 1e20;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        ivec2 q = ivec2(p + vec2(x, y) * stepPx);
        if (q.x < 0 || q.y < 0 || q.x >= res.x || q.y >= res.y) continue;
        vec3 s = taDecodeSeed(texelFetch(TA_RAW0, q, 0), TA_RAW_ENC(0));
        if (s.z > 0.5) { float d = dot(s.xy - p, s.xy - p); if (d < dBest) { dBest = d; best = s; } }
    }
    frag_color = taEncodeSeed(best.xy, best.z > 0.5, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_RESOLVE || TA_PROGRAM == TA_PROGRAM_RESOLVE_ALPHA
// seeds (TA_RAW0) and the coverage (TA_TEX0: .r, or .a for a layer target) -> the signed
// distance in px (+outside, -inside), at most p[2].x (the reach its consumers need: beyond it
// the field reads "at least that far"); p[2].y the inside threshold
void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 s = taDecodeSeed(texelFetch(TA_RAW0, ivec2(p), 0), TA_RAW_ENC(0));
    vec4 c = texelFetch(TA_TEX0, ivec2(p), 0);
    #if TA_PROGRAM == TA_PROGRAM_RESOLVE
        float a = c.r;
    #else
        float a = c.a;
    #endif
    float d = s.z > 0.5 ? min(length(s.xy - p), ta.p[2].x) : ta.p[2].x;
    frag_color = taEncodeScalar(a >= ta.p[2].y ? -d : d, TA_OUT_ENC);
}
#endif

// a 256 x n LUT row of 16-bit values in RG (texelFetch and a lerp: LUTs need not filter). A
// macro: a constructed sampler cannot be passed to a function (Vulkan GLSL)
float taLut16Value(vec4 t) { return (taByte(t.r) * 256.0 + taByte(t.g)) / 65535.0; }
int taLutIndex(float x) { return int(floor(clamp(x, 0.0, 1.0) * 255.0)); }
float taLutFraction(float x) { float f = clamp(x, 0.0, 1.0) * 255.0; return f - floor(f); }
#define taLut16(lut, row, x) mix(taLut16Value(texelFetch(lut, ivec2(taLutIndex(x), row), 0)), taLut16Value(texelFetch(lut, ivec2(min(taLutIndex(x) + 1, 255), row), 0)), taLutFraction(x))

#if TA_PROGRAM == TA_PROGRAM_HEIGHT
// Height 0..1 from the inside distance (TA_RAW0) through the bevel profile (p[2].y; 7 = the
// custom curve, TA_TEX0 row 0). Fixed: t = inside / size (p[2].x). Fit: t = inside / (k (p[2].z)
// x the local half-thickness, TA_RAW1); a rim band (p[2].w px > 0) fits as a tube
float taProfile(float t, int profile) {
    if (profile == 7) return taLut16(TA_TEX0, 0, t);                                            // custom
    if (profile == 1) { float u = 1.0 - t; return 1.0 - u * u * u; }                         // button
    if (profile == 2) { float u = 1.0 - min(t / 0.45, 1.0); return 1.0 - u * u; }             // mesa
    if (profile == 3) return t;                                                                // chamfer
    if (profile == 4) return 1.0 - abs(2.0 * t - 1.0);                                         // ridge
    if (profile == 5) return t * t;                                                            // cove
    if (profile == 6) { float f = min(t, 0.9999) * 3.0; return (floor(f) + smoothstep(0.55, 1.0, fract(f))) / 3.0; } // stepped
    return sin(t * 1.5707963);                                                                 // smooth
}
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float d = taDecodeScalar(texelFetch(TA_RAW0, ip, 0), TA_RAW_ENC(0));
    int profile = int(ta.p[2].y + 0.5);
    bool fit = ta.p[3].x > 0.5;
    float h;
    if (!fit) {
        h = taProfile(clamp(-d / ta.p[2].x, 0.0, 1.0), profile);
    } else {
        float inside = -d, size;
        float band = ta.p[2].w;
        if (band > 0.0) {
            inside = min(inside, band - inside);
            size = ta.p[2].z * 0.5 * band;
        } else {
            size = ta.p[2].z * taDecodeScalar(texelFetch(TA_RAW1, ip, 0), TA_RAW_ENC(1));
        }
        h = taProfile(clamp(inside / max(size, 0.5), 0.0, 1.0), profile);
    }
    // Photoshop's Direction Down: the height negated reads pressed in
    if (ta.p[3].y > 0.5) h = 1.0 - h;
    frag_color = taEncodeScalar(h, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_COVER_INIT
// Fit bevel, step 1: every inside pixel seeds its own inscribed circle (its centre; the radius
// is -SDF there, read back from TA_RAW0 by the next steps)
void main() {
    float d = taDecodeScalar(texelFetch(TA_RAW0, ivec2(gl_FragCoord.xy), 0), TA_RAW_ENC(0));
    frag_color = taEncodeSeed(gl_FragCoord.xy, d < 0.0, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_COVER_FLOOD || TA_PROGRAM == TA_PROGRAM_COVER_RADIUS
// the inscribed circle centred at a seed: radius -SDF (TA_RAW1) at the centre
float taCircleRadius(vec2 centre) {
    return -taDecodeScalar(texelFetch(TA_RAW1, ivec2(centre), 0), TA_RAW_ENC(1));
}
#endif

#if TA_PROGRAM == TA_PROGRAM_COVER_FLOOD
// Fit bevel, step 2 (one jump step of p[2].x): keep the largest neighbouring circle that covers
// this pixel, so each pixel ends with (about) the largest inscribed circle containing it
void main() {
    vec2 p = gl_FragCoord.xy;
    ivec2 res = textureSize(TA_RAW0, 0);
    vec3 best = taDecodeSeed(texelFetch(TA_RAW0, ivec2(p), 0), TA_RAW_ENC(0));
    float bestR = best.z > 0.5 ? taCircleRadius(best.xy) : -1.0;
    float stepPx = ta.p[2].x;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        ivec2 q = ivec2(p + vec2(x, y) * stepPx);
        if (q.x < 0 || q.y < 0 || q.x >= res.x || q.y >= res.y) continue;
        vec3 s = taDecodeSeed(texelFetch(TA_RAW0, q, 0), TA_RAW_ENC(0));
        if (s.z < 0.5) continue;
        float r = taCircleRadius(s.xy);
        if (r > bestR && length(s.xy - p) <= r + 1.0) { best = s; bestR = r; }
    }
    frag_color = taEncodeSeed(best.xy, best.z > 0.5, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_COVER_RADIUS
// Fit bevel, step 3: the covering circle's radius (the local half-thickness, px)
void main() {
    vec3 s = taDecodeSeed(texelFetch(TA_RAW0, ivec2(gl_FragCoord.xy), 0), TA_RAW_ENC(0));
    frag_color = taEncodeScalar(s.z > 0.5 ? max(taCircleRadius(s.xy), 0.0) : 0.0, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_BLUR_RAW
// separable Gaussian of a scalar field (TA_RAW0, texelFetch, clamped to the edge): p[2].xy the
// direction (whole texels), p[2].z sigma; the loop runs only the taps the radius needs
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 mx = textureSize(TA_RAW0, 0) - 1;
    ivec2 dir = ivec2(ta.p[2].xy);
    float sigma = max(ta.p[2].z, 1e-3);
    int r = int(ceil(sigma * 3.0));
    float acc = 0.0, wsum = 0.0;
    for (int i = -r; i <= r; i++) {
        float w = exp(-0.5 * float(i * i) / (sigma * sigma));
        acc += taDecodeScalar(texelFetch(TA_RAW0, clamp(p + dir * i, ivec2(0), mx), 0), TA_RAW_ENC(0)) * w;
        wsum += w;
    }
    frag_color = taEncodeScalar(acc / wsum, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_REDUCE_MAX
// the max inside distance over 8 x 8 blocks (p[2].x 1: the input is the SDF, take -d)
void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 8;
    ivec2 mx = textureSize(TA_RAW0, 0) - 1;
    bool first = ta.p[2].x > 0.5;
    float m = 0.0;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        ivec2 q = base + ivec2(x, y);
        if (q.x > mx.x || q.y > mx.y) continue;
        float v = taDecodeScalar(texelFetch(TA_RAW0, q, 0), TA_RAW_ENC(0));
        m = max(m, first ? -v : v);
    }
    frag_color = taEncodeScalar(m, TA_OUT_ENC);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_GROW
// a mask grown through its distance field (TA_RAW0) by p[2].x px, shifted by p[2].yz px (image
// sources' rims: round joins)
float taSd(ivec2 q) {
    ivec2 mx = textureSize(TA_RAW0, 0) - 1;
    return taDecodeScalar(texelFetch(TA_RAW0, clamp(q, ivec2(0), mx), 0), TA_RAW_ENC(0));
}
void main() {
    vec2 p = gl_FragCoord.xy - 0.5 - ta.p[2].yz;
    vec2 f = fract(p);
    ivec2 i = ivec2(floor(p));
    float d = mix(mix(taSd(i), taSd(i + ivec2(1, 0)), f.x), mix(taSd(i + ivec2(0, 1)), taSd(i + ivec2(1, 1)), f.x), f.y);
    frag_color = vec4(clamp(ta.p[2].x - d + 0.5, 0.0, 1.0));
}
#endif

#if TA_PROGRAM == TA_PROGRAM_MASK_KEEP
// かすれ: an element's mask (TA_TEX0) x the shared keep-mask (TA_TEX1)
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    frag_color = vec4(texelFetch(TA_TEX0, p, 0).r * texelFetch(TA_TEX1, p, 0).r);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_ERODE
// かすれ (dry brush) keep-mask, 1 = ink kept, from the union of every element (TA_TEX0, linear)
// and its distance (TA_RAW0). The brush is dragged along p[2].xy: bristle lanes run along it and
// the dry ones lose ink over a random length measured along the drag from the silhouette (more
// at the trailing edge), so gaps are streaks entering from stroke ends; dry lanes also split
// off along side edges. The distance caps everything to a band of the edge size. Every cut is
// anti-aliased through its own gradient (the elements' bevels would light an aliased step).
// p[2].zw amount, edge px; p[3] = (length px, lane px, seed x, seed y); p[4].xy the frame
// corner from the layer origin, frame px.
float taUnion(vec2 q) { return texture(TA_TEX0, q / vec2(textureSize(TA_TEX0, 0))).r; }
float taExitDist(vec2 p, vec2 dir, float maxD) {
    float stepL = max(maxD / 48.0, 0.75);
    float t = 0.0;
    for (int i = 0; i < 96; i++) {
        float tn = t + stepL;
        if (tn > maxD) return maxD;
        if (taUnion(p + dir * tn) < 0.5) {
            float lo = t, hi = tn;
            for (int k = 0; k < 6; k++) { float m = 0.5 * (lo + hi); if (taUnion(p + dir * m) < 0.5) hi = m; else lo = m; }
            return 0.5 * (lo + hi);
        }
        t = tn;
    }
    return maxD;
}
float taSdAt(ivec2 q) {
    return taDecodeScalar(texelFetch(TA_RAW0, clamp(q, ivec2(0), textureSize(TA_RAW0, 0) - 1), 0), TA_RAW_ENC(0));
}
vec2 taSdGrad(vec2 q) {
    ivec2 i = ivec2(q);
    return vec2(taSdAt(i + ivec2(1, 0)) - taSdAt(i - ivec2(1, 0)), taSdAt(i + ivec2(0, 1)) - taSdAt(i - ivec2(0, 1)));
}
// the gradient of taExitDist(p, dir) per frame px
vec2 taExitGrad(vec2 p, vec2 dir, float d, float maxD) {
    if (d >= maxD) return vec2(0.0);
    vec2 n = taSdGrad(p + dir * d);
    float len = length(n);
    if (len < 1e-4) return vec2(0.0);
    float c = dot(dir, n);
    return -n / ((c < 0.0 ? -1.0 : 1.0) * max(abs(c), 0.05 * len));
}
// x lane dryness (0 inked, 1 driest), y ink lost along the drag (px), z skip noise
vec3 taBrush(vec2 p) {
    vec2 dir = ta.p[2].xy;
    float amount = ta.p[2].z;
    vec2 perp = vec2(-dir.y, dir.x);
    vec2 pl = p + ta.p[4].xy;
    float lane = dot(pl, perp) / ta.p[3].y + ta.p[3].z;
    float along = dot(pl, dir) / ta.p[3].x + ta.p[3].w;
    float dry = 0.55 * vnoise(vec2(lane, along * 0.15)) + 0.3 * vnoise(vec2(lane * 0.21, 3.7 + along * 0.1)) + 0.15 * vnoise(vec2(lane * 2.7, along * 0.8));
    float x = clamp((amount - dry) / max(amount, 1e-3), 0.0, 1.0);
    float gap = ta.p[3].x * x * (0.45 + 0.8 * vnoise(vec2(lane * 0.37 + 11.0, along * 1.7)));
    return vec3(x, gap, vnoise(vec2(lane * 0.9 + 5.0, along * 4.0)));
}
float taCut(float e, vec2 g) { float w = 0.7 * max(length(g), 1.0); return smoothstep(-w, w, e); }
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2 p = gl_FragCoord.xy;
    vec2 dir = ta.p[2].xy;
    float edgePx = ta.p[2].w, lenPx = ta.p[3].x;
    float depth = -taSdAt(ip);
    if (texelFetch(TA_TEX0, ip, 0).r <= 0.0 || depth > edgePx) { frag_color = vec4(1.0); return; }
    vec3 b = taBrush(p);
    float x = b.x;
    if (x <= 0.0) { frag_color = vec4(1.0); return; }
    vec3 bx = taBrush(p + vec2(1.0, 0.0)) - b, by = taBrush(p + vec2(0.0, 1.0)) - b;
    vec2 gX = vec2(bx.x, by.x), gGap = vec2(bx.y, by.y), gSkip = vec2(bx.z, by.z);
    float gap = b.y;
    float maxD = min(lenPx * 1.3, edgePx * 3.0);
    float dF = taExitDist(p, dir, maxD);
    float dB = taExitDist(p, -dir, maxD);
    float eat = max(taCut(gap - dF, gGap - taExitGrad(p, dir, dF, maxD)), taCut(gap * 0.35 - dB, gGap * 0.35 - taExitGrad(p, -dir, dB, maxD)));
    eat = max(eat, taCut(x * x * edgePx * 0.3 - depth, 2.0 * x * edgePx * 0.3 * gX + 0.5 * taSdGrad(p)));
    float sw = max(0.125, 0.7 * length(gSkip));
    eat *= mix(1.0, smoothstep(0.325 - sw, 0.325 + sw, b.z), 0.45 * (1.0 - x));
    eat *= 1.0 - smoothstep(edgePx * 0.55, edgePx, depth);
    frag_color = vec4(1.0 - eat);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_SPAN
// the index of the glyph nearest to each frame pixel: the smallest distance to its ink box (0
// inside), ties by the distance to the box centre. TA_RAW0: RGBA32F N x 2 boxes (row 0 ink
// boxes, row 1 line boxes, layer units); p[2] = (count, frame px per layer unit, frame min x, y)
void main() {
    int count = int(ta.p[2].x + 0.5);
    float m = ta.p[2].y;
    vec2 p = ta.p[2].zw + gl_FragCoord.xy / m;
    float best = 1e30, bestC = 1e30;
    int bi = 0;
    for (int i = 0; i < count; i++) {
        vec4 b = texelFetch(TA_RAW0, ivec2(i, 0), 0);
        vec2 c = 0.5 * (b.xy + b.zw);
        vec2 q = abs(p - c) - 0.5 * (b.zw - b.xy);
        float d = length(max(q, 0.0));
        float dc = dot(p - c, p - c);
        if (d < best - 1e-4 || (d <= best + 1e-4 && dc < bestC)) { best = d; bestC = dc; bi = i; }
    }
    frag_color = taEncodeScalar(float(bi), TA_OUT_ENC);
}
#endif
