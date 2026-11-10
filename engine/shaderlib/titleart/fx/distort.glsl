// S83's distortions (§6.6.12 Wave Warp, §6.6.13 Chromatic Aberration, §6.6.15 Glitch, §6.6.8
// Motion Trail), ported from Pachislop src/effects/a/distort.ts (wave, chromaticAberration),
// finish.ts (glitch) and stylize.ts (motionTrail) by SafEight, used with the author's permission.
// Wave Warp's turbulent type and Glitch's blocks mode are S83's additions.

#ifndef TITLEART_FX_DISTORT_GLSL
#define TITLEART_FX_DISTORT_GLSL

// Wave Warp: a sine displacement across a direction (flag), circular ripples from a centre, or a
// turbulent fbm field. TA_TEX0 the source. p2 = (direction (frame px, y down), amplitude px,
// wavelength canvas px), p3 = (phase 0..1, type 0 linear 1 radial 2 turbulent, octaves,
// evolution), p4 = (ripple centre target px, canvas centre target px)
vec4 tfx_waveWarp() {
    vec2 p = gl_FragCoord.xy;
    vec2 dir = ta.p[2].xy;
    float amp = ta.p[2].z, lambda = ta.p[2].w, phase = ta.p[3].x;
    int type = int(ta.p[3].y + 0.5);
    vec2 disp;
    if (type == 0) {
        float ph = 2.0 * TA_PI * (dot(tfx_canvasPx(p, ta.p[4].zw), dir) / lambda - phase);
        // Pachislop's perpendicular (y up) in frame rows
        disp = vec2(dir.y, -dir.x) * amp * sin(ph);
    } else if (type == 1) {
        vec2 d = p - ta.p[4].xy;
        float r = length(d);
        float ph = 2.0 * TA_PI * (r / TA_PX / lambda - phase);
        disp = (r > 1e-3 ? d / r : vec2(0.0)) * amp * sin(ph);
    } else {
        // turbulent: a 2D fbm field whose domain drifts with Evolution and circles with Phase (so
        // phase 0 and 1 are the same frame)
        int octaves = int(ta.p[3].z + 0.5);
        float a = 2.0 * TA_PI * phase;
        vec2 q = tfx_canvasPx(p, ta.p[4].zw) / lambda + ta.p[3].w * vec2(0.31, 0.17) + 0.5 * vec2(cos(a), sin(a));
        disp = amp * 2.0 * (vec2(tfx_fbm(q, octaves), tfx_fbm(q + vec2(5.2, 1.3), octaves)) - 0.5);
    }
    vec2 q = (p - disp) / TA_SIZE;
    vec4 frame = ta_frameUv();
    return (q.x < frame.x || q.y < frame.y || q.x > frame.z || q.y > frame.w) ? vec4(0.0) : texture(TA_TEX0, q);
}

// Chromatic Aberration: red and blue pushed apart radially from a centre or along a direction,
// with an optional triangular spectral smear of n taps. TA_TEX0 the source.
// p2 = (direction (y down), amount px, n), p3 = (centre target px, type 0 radial 1 directional)
vec4 tfx_chromatic() {
    vec2 p = gl_FragCoord.xy;
    vec2 frameSize = (ta_frameUv().zw - ta_frameUv().xy) * TA_SIZE;
    float amount = ta.p[2].z;
    int n = int(ta.p[2].w + 0.5);
    vec2 off = ta.p[3].z < 0.5 ? (p - ta.p[3].xy) / (0.5 * length(frameSize)) * amount : ta.p[2].xy * amount;
    vec3 sumC = vec3(0.0), sumA = vec3(0.0), sumW = vec3(0.0);
    for (int i = 0; i < 32; i++) {
        if (i >= n) break;
        float k = -1.0 + 2.0 * float(i) / float(max(n - 1, 1));
        vec3 w = max(1.0 - abs(vec3(k - 1.0, k, k + 1.0)), 0.0);
        vec4 s = textureLod(TA_TEX0, (p + off * k) / TA_SIZE, 0.0);
        sumC += s.rgb * w;
        sumA += s.a * w;
        sumW += w;
    }
    vec3 a = sumA / max(sumW, vec3(1e-6));
    return vec4(sumC / max(sumW, vec3(1e-6)), max(a.r, max(a.g, a.b)));
}

// Glitch: horizontal slices (fractions of the layer box's height, so the pattern is the same at
// every scale) or rectangular blocks jump sideways, each with its own RGB split; premultiplied
// channels from three shifted copies, so a lone copy shows as a pure R, G or B fringe.
// TA_TEX0 the source, TA_RAW1 the bounds. p2 = (amount, slices, max offset px, split px),
// p3 = (seed, mode 0 slices 1 blocks, block size px)
vec4 tfx_glitch() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    float amount = ta.p[2].x, slices = ta.p[2].y, offset = ta.p[2].z, splitPx = ta.p[2].w;
    float seed = ta.p[3].x;
    vec4 b = fx_layerBox();
    float id;
    if (ta.p[3].y < 0.5) {
        // Pachislop's slices count up from the box's bottom
        float t = (b.w - p.y) / max(b.w - b.y, 1e-4);
        float f = t * slices;
        float i = floor(f);
        id = i * 2.0 + step(0.25 + 0.5 * hash12(vec2(i, seed)), fract(f));
    } else {
        // blocks twice as wide as tall, each row shifted by its own fraction of a block
        float bs = max(ta.p[3].z, 1.0);
        float row = floor((p.y - b.y) / (0.5 * bs));
        float col = floor((p.x - b.x) / bs + hash12(vec2(row, seed + 5.3)));
        id = col * 127.1 + row * 311.7;
    }
    float h1 = hash12(vec2(id, seed + 3.1)), h2 = hash12(vec2(id, seed + 7.7)), h3 = hash12(vec2(id, seed + 11.3));
    float on = step(1.0 - amount, h1);
    float off = on * (h2 * 2.0 - 1.0) * offset;
    float split = splitPx * (0.4 + 0.6 * h3) * mix(0.35, 1.0, on);
    vec2 px = vec2(1.0 / TA_SIZE.x, 0.0);
    vec4 cR = texture(TA_TEX0, uv - (off + split) * px);
    vec4 cG = texture(TA_TEX0, uv - off * px);
    vec4 cB = texture(TA_TEX0, uv - (off - split) * px);
    return vec4(cR.r, cG.g, cB.b, max(cG.a, max(cR.a, cB.a)));
}

// Motion Trail, smear: the brightest of the shape's copies along the direction, scaled about the
// layer box's centre (taper) and fading toward the end, tinted, behind the layer.
// TA_TEX0 L, TA_RAW1 the bounds. p2 = (direction (y down), length px, step px),
// p3 = (tint linear, tint amount), p4 = (fade, taper, opacity)
vec4 tfx_motionSmear() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec4 b = fx_layerBox();
    vec2 c = 0.5 * (b.xy + b.zw);
    vec2 p = gl_FragCoord.xy;
    vec2 dir = ta.p[2].xy;
    float len = ta.p[2].z, stepPx = ta.p[2].w;
    float fade = ta.p[4].x, taper = ta.p[4].y;
    vec4 best = vec4(0.0);
    float bestF = 0.0;
    int n = int(ceil(len / stepPx));
    for (int i = 1; i <= 1024; i++) {
        if (i > n) break;
        float s = min(float(i) * stepPx, len);
        float f = s / len;
        float k = max(1.0 - taper * f, 0.02);
        vec2 q = c + (p - dir * s - c) / k;
        vec4 v = textureLod(TA_TEX0, q / TA_SIZE, 0.0) * pow(1.0 - f, fade);
        if (v.a > best.a) {
            best = v;
            bestF = f;
        }
    }
    vec3 cs = ta_unpremul(best);
    cs = mix(cs, ta.p[3].rgb, ta.p[3].w * mix(0.35, 1.0, bestF));
    return L + vec4(cs * best.a, best.a) * ta.p[4].z * (1.0 - L.a);
}

// Motion Trail, echo (残像): copies stepped along the direction, each scaled about the box
// centre, fading by the decay per copy and tinted from the start to the end colour; per-copy blur
// grows with the copy index, read from three pre-blurred levels. TA_TEX0 L, TA_TEX1..3 the
// blurred levels (1/3, 2/3, all of the farthest copy's radius), TA_RAW1 the bounds.
// p2 = (direction (y down), copy offset px, scale per copy), p3 = (start colour linear, decay),
// p4 = (end colour linear, tint amount), p5 = (copies, blurred 0/1, opacity)
vec4 tfx_echoLevel(vec2 uv, float t, bool blurred) {
    if (!blurred) return textureLod(TA_TEX0, uv, 0.0);
    vec4 a = textureLod(TA_TEX0, uv, 0.0), b = textureLod(TA_TEX1, uv, 0.0), c = textureLod(TA_TEX2, uv, 0.0), d = textureLod(TA_TEX3, uv, 0.0);
    return t < 1.0 ? mix(a, b, t) : t < 2.0 ? mix(b, c, t - 1.0) : mix(c, d, t - 2.0);
}
vec4 tfx_motionEcho() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec4 b = fx_layerBox();
    vec2 c = 0.5 * (b.xy + b.zw);
    vec2 p = gl_FragCoord.xy;
    vec2 dir = ta.p[2].xy;
    float off = ta.p[2].z, scale = ta.p[2].w;
    int n = int(ta.p[5].x + 0.5);
    bool blurred = ta.p[5].y > 0.5;
    vec4 frame = ta_frameUv();
    vec4 acc = vec4(0.0);
    for (int i = 12; i >= 1; i--) {
        if (i > n) continue;
        float fi = float(i);
        vec2 q = (c + (p - dir * off * fi - c) / pow(scale, fi)) / TA_SIZE;
        vec4 v = tfx_echoLevel(q, 3.0 * fi / float(n), blurred);
        if (any(lessThan(q, frame.xy)) || any(greaterThan(q, frame.zw))) v = vec4(0.0);
        float t = n > 1 ? (fi - 1.0) / float(n - 1) : 0.0;
        vec3 cs = mix(ta_unpremul(v), mix(ta.p[3].rgb, ta.p[4].rgb, t), ta.p[4].w);
        float a = v.a * pow(1.0 - ta.p[3].w, fi - 1.0);
        acc = tfx_over(vec4(cs * a, a), acc);
    }
    return L + acc * ta.p[5].z * (1.0 - L.a);
}

#endif // TITLEART_FX_DISTORT_GLSL
