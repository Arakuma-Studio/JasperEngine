#version 450
precision highp float;
precision highp int;

// The 3D title glint (docs/specs/S86-title-3d.md R32; ShaderType::TITLE_GLINT, the pass in
// TG_PROGRAM), fullscreen triangles of titleart.vert addressing their target by gl_FragCoord in
// memory rows (uv = gl_FragCoord.xy x tg.p3.xy, the target's texel size). Ported from Pachislop
// src/engine/three/glint.ts by SafEight, used with the author's permission: the bright pass on a
// grid locked to canvas pixels, box mips, the hot coverage that tames broad faces, the narrow and
// wide blooms, the point detector (the bright pass minus the larger of its 1D blurs), the pinch
// that keeps real glints, the star budget, and the Kawase streaks per ray direction; the sum,
// tinted and scaled, is light added before the tone curve (title3d_post.frag TONEMAP).
// - 1 BRIGHT: the main HDR image (compressed light decoded, the MSAA shoulder expanded)
//   box-averaged onto the grid, then thresholded with a soft knee (e^2 / (e + 4)).
// - 2 DOWN: a bilinear tap at the destination texel's centre (a 2 x 2 box on exact halvings).
// - 3 BLUR: a 1D gaussian along tg.p1.xy (uv per texel), sigma p1.z, radius p1.w texels.
// - 4 STREAK: 4 taps back along p1.xy with weights falling by p1.z (no light outside the frame).
// - 5 ADD: the source at uv x p2.xy + p2.zw times the gain p1.rgb (additive into the output).
// - 6 HIGHPASS: max(src - max(blur x, blur y), 0) x K / (K + coverage).
// - 7 TAME: src x K / (K + coverage).
// - 8 PINCH: src x (src / (blur K))^2 x (blur / (coverage Kc))^2, each at most 1.
// - 9 DENSITY: src x ((near / (wide K))^2)^2, at most 1.

uniform texture2D u_tgSrc;
uniform sampler u_tgSrc_smp;
uniform texture2D u_tgAux;
uniform sampler u_tgAux_smp;
uniform texture2D u_tgAux2;
uniform sampler u_tgAux2_smp;
uniform texture2D u_tgAux3;
uniform sampler u_tgAux3_smp;

uniform u_fs_titleGlint {
    vec4 p0;    // BRIGHT: y exposure, z 1 = expand the shoulder, w 1 = decode compressed light
    vec4 p1;    // per program (above)
    vec4 p2;    // BRIGHT: xy the source texel of the grid's first corner; ADD: uv scale and offset
    vec4 p3;    // xy: 1 / the target's size (texels)
} tg;

out vec4 frag_color;

#include "titleart/hdr_compress.glsl"

float tgMax(vec3 c) {
    return max(c.r, max(c.g, c.b));
}

void main() {
    vec2 uv = gl_FragCoord.xy * tg.p3.xy;
#if TG_PROGRAM == 1
    // box average first (premultiplied), threshold after: the knee sees the same footprint at
    // every export scale; texels past the source are empty
    int factor = int(tg.p1.y + 0.5);
    ivec2 p = ivec2(tg.p2.xy) + ivec2(gl_FragCoord.xy) * factor;
    ivec2 size = textureSize(sampler2D(u_tgSrc, u_tgSrc_smp), 0);
    vec4 c = vec4(0.0);
    for (int y = 0; y < 64; y++) {
        if (y >= factor) break;
        for (int x = 0; x < 64; x++) {
            if (x >= factor) break;
            ivec2 q = p + ivec2(x, y);
            if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, size))) {
                vec4 s = texelFetch(sampler2D(u_tgSrc, u_tgSrc_smp), q, 0);
                if (tg.p0.w > 0.5) s = hd_decodeLight(s);
                if (tg.p0.z > 0.5 && s.a > 1e-5) s.rgb = hd_expand(s.rgb / s.a, tg.p0.y) * s.a;
                c += s;
            }
        }
    }
    c /= float(factor * factor);
    frag_color = vec4(0.0, 0.0, 0.0, 1.0);
    if (c.a <= 1e-4) return;
    float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) / c.a;
    float e = min(l, 64.0) - tg.p1.x;
    if (e <= 0.0) return;
    frag_color.rgb = c.rgb * (e * e / (e + 4.0)) / max(l, 1e-4);
#elif TG_PROGRAM == 2
    frag_color = vec4(texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv).rgb, 1.0);
#elif TG_PROGRAM == 3
    int radius = int(tg.p1.w + 0.5);
    float sigma = tg.p1.z;
    vec3 s = vec3(0.0);
    float ws = 0.0;
    for (int i = 0; i < 257; i++) {
        int o = i - radius;
        if (o > radius) break;
        float x = float(o);
        float w = exp(-0.5 * x * x / (sigma * sigma));
        s += texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv + tg.p1.xy * x).rgb * w;
        ws += w;
    }
    frag_color = vec4(s / ws, 1.0);
#elif TG_PROGRAM == 4
    vec3 s = vec3(0.0);
    float ws = 0.0;
    float w = 1.0;
    for (int i = 0; i < 4; i++) {
        vec2 q = uv - tg.p1.xy * float(i);
        // outside the frame there is no light (clamp-to-edge would smear the border)
        float inside = step(0.0, q.x) * step(q.x, 1.0) * step(0.0, q.y) * step(q.y, 1.0);
        s += texture(sampler2D(u_tgSrc, u_tgSrc_smp), q).rgb * w * inside;
        ws += w;
        w *= tg.p1.z;
    }
    frag_color = vec4(s / ws, 1.0);
#elif TG_PROGRAM == 5
    frag_color = vec4(texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv * tg.p2.xy + tg.p2.zw).rgb * tg.p1.rgb, 0.0);
#elif TG_PROGRAM == 6
    vec3 b = texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv).rgb;
    vec3 c = texture(sampler2D(u_tgAux3, u_tgAux3_smp), uv).rgb;
    float tame = tg.p1.x / (tg.p1.x + tgMax(c));
    vec3 blurred = max(texture(sampler2D(u_tgAux, u_tgAux_smp), uv).rgb, texture(sampler2D(u_tgAux2, u_tgAux2_smp), uv).rgb);
    frag_color = vec4(max(b - blurred, vec3(0.0)) * tame, 1.0);
#elif TG_PROGRAM == 7
    vec3 c = texture(sampler2D(u_tgAux3, u_tgAux3_smp), uv).rgb;
    frag_color = vec4(texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv).rgb * (tg.p1.x / (tg.p1.x + tgMax(c))), 1.0);
#elif TG_PROGRAM == 8
    vec3 p = texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv).rgb;
    vec3 b = texture(sampler2D(u_tgAux, u_tgAux_smp), uv).rgb;
    vec3 c = texture(sampler2D(u_tgAux3, u_tgAux3_smp), uv).rgb;
    float k = clamp(tgMax(p) / max(tgMax(b) * tg.p1.x, 1e-6), 0.0, 1.0);
    float iso = clamp(tgMax(b) / max(tgMax(c) * tg.p1.y, 1e-6), 0.0, 1.0);
    frag_color = vec4(p * (k * k) * (iso * iso), 1.0);
#elif TG_PROGRAM == 9
    vec3 n = texture(sampler2D(u_tgAux, u_tgAux_smp), uv).rgb;
    vec3 w = texture(sampler2D(u_tgAux2, u_tgAux2_smp), uv).rgb;
    float g = clamp(tgMax(n) / max(tgMax(w) * tg.p1.x, 1e-6), 0.0, 1.0);
    g *= g;
    frag_color = vec4(texture(sampler2D(u_tgSrc, u_tgSrc_smp), uv).rgb * g * g, 1.0);
#endif
}
