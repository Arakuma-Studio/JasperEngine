// S83's blurs (§6.6.3 Motion Blur, §6.6.4 Lens Blur, §6.6.5 Tilt-Shift Blur, §6.6.6 Radial
// Blur), ported from Pachislop src/effects/a/blur.ts (motionBlur, lensBlur, tiltShift) and
// distort.ts (radialBlur) by SafEight, used with the author's permission. Every loop samples with
// textureLod (D3D11 has no implicit derivatives in a loop of data-dependent length).

#ifndef TITLEART_FX_BLUR_GLSL
#define TITLEART_FX_BLUR_GLSL

// The average of n taps along dir (uv per render px) at start + i x spacing render px.
// TA_TEX0 the source. p2 = (dir uv xy, start px, spacing px), p3.x = n (<= 64)
vec4 tfx_motionBlur() {
    vec2 uv = ta_uv();
    vec2 dir = ta.p[2].xy;
    float start = ta.p[2].z, spacing = ta.p[2].w;
    int n = int(ta.p[3].x + 0.5);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 64; i++) {
        if (i >= n) break;
        acc += textureLod(TA_TEX0, uv + dir * (start + spacing * float(i)), 0.0);
    }
    return acc / float(max(n, 1));
}

// the highlight weight of a premultiplied colour: 1 where its sRGB luma is at or above the threshold
float tfx_highlight(vec4 c, float thr) {
    return c.a * smoothstep(thr - 0.04, thr, ta_luma(ta_toSrgb(ta_unpremul(c))));
}

// 2x downsample weighted by highlights, so a bright point survives as a bright texel and still
// blooms at low resolution. TA_TEX0 the finer level. p2 = (its texel uv, threshold, gain)
vec4 tfx_lensDown() {
    vec2 uv = ta_uv();
    vec2 texel = ta.p[2].xy;
    float thr = ta.p[2].z, gain = ta.p[2].w;
    vec4 acc = vec4(0.0);
    float ws = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 off = vec2(float(i - (i / 2) * 2), float(i / 2)) - 0.5;
        vec4 c = textureLod(TA_TEX0, uv + off * texel, 0.0);
        float w = 1.0 + gain * tfx_highlight(c, thr);
        acc += c * w;
        ws += w;
    }
    return acc / ws;
}

// The gather over the iris: n taps on a Vogel (golden-angle) spiral, uniform over the unit disc,
// pushed out to the polygon by P(theta) (apothem / cos) and weighted by P^2 so the polygon is
// covered evenly; highlights weigh (1 + gain). The spiral is the same for every pixel, so the
// kernel is fixed and smooth. TA_TEX0 the level. p2 = (radius uv xy, n <= 512, blades),
// p3 = (rotation rad, threshold, gain)
vec4 tfx_lensGather() {
    vec2 uv = ta_uv();
    vec2 rad = ta.p[2].xy;
    int n = int(ta.p[2].z + 0.5);
    float blades = ta.p[2].w, rot = ta.p[3].x, thr = ta.p[3].y, gain = ta.p[3].z;
    float seg = blades > 0.0 ? 2.0 * TA_PI / blades : 0.0;
    vec4 acc = vec4(0.0);
    float ws = 0.0;
    for (int i = 0; i < 512; i++) {
        if (i >= n) break;
        float rho = sqrt((float(i) + 0.5) / float(n));
        float th = float(i) * 2.39996323;
        float P = 1.0;
        if (seg > 0.0) P = cos(0.5 * seg) / cos(mod(th - rot, seg) - 0.5 * seg);
        vec4 c = textureLod(TA_TEX0, uv + vec2(cos(th), sin(th)) * (rho * P) * rad, 0.0);
        float w = P * P * (1.0 + gain * tfx_highlight(c, thr));
        acc += c * w;
        ws += w;
    }
    return acc / max(ws, 1e-6);
}

// The full-resolution upsample plus Photoshop's Noise: +-noise (sRGB levels / 255) uniform, or
// Gaussian with sigma = noise / 2, per channel or monochrome, only where there is ink, keyed on
// the whole render's pixel and the seed. TA_TEX0 the blurred level.
// p2 = (noise, gaussian 0/1, mono 0/1, seed)
vec4 tfx_lensFinish() {
    vec4 c = texture(TA_TEX0, ta_uv());
    float noise = ta.p[2].x;
    if (noise > 0.0 && c.a > 1e-4) {
        vec2 p = floor(gl_FragCoord.xy - ta_frameUv().xy * TA_SIZE) + ta.p[2].w * 101.3;
        vec3 u = vec3(hash12(p), hash12(p + 31.7), hash12(p + 73.1));
        vec3 nv = u * 2.0 - 1.0;
        if (ta.p[2].y > 0.5) {
            vec3 v = vec3(hash12(p + 11.3), hash12(p + 47.9), hash12(p + 91.4));
            nv = 0.5 * sqrt(-2.0 * log(max(u, 1e-6))) * cos(2.0 * TA_PI * v);
        }
        if (ta.p[2].z > 0.5) nv = vec3(nv.x);
        c.rgb = ta_toLinear(ta_toSrgb(ta_unpremul(c)) + nv * noise) * c.a;
    }
    return c;
}

// tilt-shift's blur amount 0..8 (in eighths of the maximum) at this pixel: the distance to the
// focus line beyond half the sharp width, over the falloff, eased
float tfx_tiltAmount(vec2 c, vec2 nrm, float halfWidth, float fall) {
    float d = abs(dot(gl_FragCoord.xy - c, nrm));
    float t = fall > 0.0 ? clamp((d - halfWidth) / fall, 0.0, 1.0) : step(halfWidth, d);
    return 8.0 * t * t * (3.0 - 2.0 * t);
}

// The two widest levels mixed where the band blurs most (tilt-shift reads five images; a pass
// binds four, so the far half is mixed first). TA_TEX0 the sigma/2 level, TA_TEX1 the sigma level.
// p2 = (focus centre target px, half width px, falloff px), p3.xy = the band's normal
vec4 tfx_tiltFar() {
    vec2 uv = ta_uv();
    float x = tfx_tiltAmount(ta.p[2].xy, ta.p[3].xy, ta.p[2].z, ta.p[2].w);
    vec4 l3 = texture(TA_TEX0, uv);
    float l = log2(max(x, 1.0));
    return l > 2.0 ? mix(l3, texture(TA_TEX1, uv), l - 2.0) : l3;
}

// The blend between the sharp layer and the Gaussian levels at sigma x {1/8, 1/4, 1/2, 1}:
// linear below 1/8, log2 above. TA_TEX0 the layer, TA_TEX1 the 1/8 level, TA_TEX2 the 1/4 level,
// TA_TEX3 tilt_far's mix. p2, p3 as tfx_tiltFar
vec4 tfx_tiltBlend() {
    vec2 uv = ta_uv();
    float x = tfx_tiltAmount(ta.p[2].xy, ta.p[3].xy, ta.p[2].z, ta.p[2].w);
    if (x <= 1.0) return mix(texture(TA_TEX0, uv), texture(TA_TEX1, uv), x);
    float l = log2(x);
    if (l <= 1.0) return mix(texture(TA_TEX1, uv), texture(TA_TEX2, uv), l);
    return mix(texture(TA_TEX2, uv), texture(TA_TEX3, uv), clamp(l - 1.0, 0.0, 1.0));
}

// Radial blur: n taps over a span, zoom on a log scale (q = c + (p - c) e^(-span k)), spin over
// an angle centred on 0, with a per-pixel jitter keyed on the whole render's pixel. TA_TEX0 the
// source, TA_TEX1 the original (Original On Top). p2 = (centre target px, span, n <= 64),
// p3 = (type 0 zoom 1 spin, keep original 0/1)
vec4 tfx_radialBlur() {
    vec2 p = gl_FragCoord.xy;
    vec2 c = ta.p[2].xy;
    float span = ta.p[2].z;
    int n = int(ta.p[2].w + 0.5);
    int type = int(ta.p[3].x + 0.5);
    float j = hash12(p - ta_frameUv().xy * TA_SIZE + span);
    vec4 acc = vec4(0.0);
    vec2 d = p - c;
    for (int i = 0; i < 64; i++) {
        if (i >= n) break;
        float k = (float(i) + j) / float(n);
        vec2 q;
        if (type == 0) {
            q = c + d * exp(-span * k);
        } else {
            float a = span * (k - 0.5);
            float cs = cos(a), sn = sin(a);
            q = c + vec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
        }
        acc += textureLod(TA_TEX0, q / TA_SIZE, 0.0);
    }
    acc /= float(max(n, 1));
    vec4 L = texture(TA_TEX1, ta_uv());
    return ta.p[3].y > 0.5 ? L + acc * (1.0 - L.a) : acc;
}

#endif // TITLEART_FX_BLUR_GLSL
