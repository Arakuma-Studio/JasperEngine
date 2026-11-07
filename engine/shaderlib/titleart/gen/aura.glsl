// Aura, ported from Pachislop src/generators/energy/corona.ts (coronaFrag, auraFrag) by SafEight,
// used with the author's permission: energy tendrils radiating from the edges of the followed
// layer's content, or an aura of flickering tongues hugging its silhouette (outward along the
// edge normals, or rising with the upstream distance, tfx_upstream.glsl), drawn as light.
//
// Normals: Pachislop takes central differences 3 px apart of a blurred distance field, so the
// streaks bend smoothly across the creases between letters. The blur is linear, so the same
// differences taken first (AuraNormals, packed into colour as 0.5 + d/12: a distance field
// changes by at most 6 px over them) and blurred after (fx::blur) give that gradient, and the
// packing stays exact on the RGBA8 path where a blurred distance would not fit.
//
// Drawing space: document space (canvas px, +y up); the field is target px. TA_TEX0 the gradient
// (edge -> tips), TA_TEX1 the blurred differences, TA_RAW0 the content's distance field.
// AuraTendrils: p6 = (length, wispiness, phase, intensity), p7 = (density, seed, edge glow,
// mobile 0/1). AuraRising: p6 = (tongue length, turbulence scale, breakup, phase), p7 =
// (intensity, seed, edge glow, strong 0/1), p8 = (rising 0/1, mobile 0/1, 0, 0). Lengths and
// scales in canvas px. Mobile (§6.7.13, the mobile tier at half resolution) takes 3-octave fbm.

#ifndef TITLEART_GEN_AURA_GLSL
#define TITLEART_GEN_AURA_GLSL

#include "titleart/gen/tfx_upstream.glsl"

// the mobile tier's fbm: fbm's first three octaves plus the mean of the two it drops, so the
// tongues keep their lengths and only the finest detail goes
float aura_fbm3(vec2 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 3; i++) {
        s += a * vnoise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return s + (0.0625 + 0.03125) * 0.5;
}

float aura_fbm(vec2 p, bool mobile) {
    return mobile ? aura_fbm3(p) : fbm(p);
}

float aura_ridge(vec2 p) {
    return 1.0 - abs(2.0 * vnoise(p) - 1.0);
}

// AuraNormals: the field's central differences 3 target px apart (edges clamped, as Pachislop's
// clamped texture reads), packed
vec4 gen_auraNormals() {
    vec2 f = gl_FragCoord.xy;
    vec2 lo = vec2(0.5), hi = TA_SIZE - 0.5;
    float gx = gen_sd(clamp(f + vec2(3.0, 0.0), lo, hi)) - gen_sd(clamp(f - vec2(3.0, 0.0), lo, hi));
    float gy = gen_sd(clamp(f + vec2(0.0, 3.0), lo, hi)) - gen_sd(clamp(f - vec2(0.0, 3.0), lo, hi));
    return vec4(clamp(0.5 + vec2(gx, gy) / 12.0, 0.0, 1.0), 0.0, 1.0);
}

// the outward normal from the blurred differences, as a drawing-space direction (y up): the
// target-px gradient carried through the inverse map and scaled back to target px, so the
// 1e-6 Pachislop adds before normalizing weighs the same
vec2 aura_normal() {
    vec2 g = (texture(TA_TEX1, ta_uv()).rg - 0.5) * 12.0;
    vec2 s = vec2(ta.p[2].x * g.x + ta.p[2].y * g.y, ta.p[2].z * g.x + ta.p[2].w * g.y) * gen_spaceScale();
    return normalize(s + 1e-6);
}

vec4 gen_auraTendrils() {
    float len = ta.p[6].x, wisp = ta.p[6].y, tt = ta.p[6].z, intensity = ta.p[6].w;
    float density = ta.p[7].x, seedN = ta.p[7].y, glowAmount = ta.p[7].z;
    bool mobile = ta.p[7].w > 0.5;
    float px = gen_spaceScale();

    float d = gen_sd(gl_FragCoord.xy);
    vec2 nrm = aura_normal();
    float dc = max(d, 0.0) / px;
    vec2 pc = gen_spacePos();
    vec2 seed = vec2(seedN * 13.1, seedN * 5.7);
    // Nearest edge point: constant along each outward normal -> radial streaks
    vec2 q = pc - nrm * dc;
    // Curl grows with distance so tendrils bend and wisp out toward their tips
    vec2 wp = pc / (len * 0.7) + seed;
    vec2 warp = vec2(aura_fbm(wp + vec2(0.0, -tt * 0.3), mobile), aura_fbm(wp + vec2(4.3, 2.1 - tt * 0.3), mobile)) - 0.5;
    vec2 qw = q + warp * wisp * dc * 1.1;
    float k = dc / len;
    float sc = 0.16 / density;
    float t1 = pow(aura_ridge(qw / (len * sc) + vec2(tt * 0.25, -tt * 0.15) + seed), 6.0);
    float t2 = pow(aura_ridge(qw / (len * sc * 0.4) + vec2(-tt * 0.4, tt * 0.25) + seed.yx), 14.0);
    // Pulses travelling outward along each tendril
    float pulse = 0.55 + 0.45 * vnoise(vec2(k * 4.0 - tt * 1.6, dot(q, vec2(0.013, 0.017)) * density * 8.0) + seed);
    float fall = pow(clamp(1.0 - k, 0.0, 1.0), 1.6);
    float tend = (t1 * 0.9 + t2 * 0.7) * pulse * fall;
    float glow = exp(-k * 7.0) * glowAmount;
    float inner = d < 0.0 ? exp(d / (len * 0.06 * px)) : 1.0;
    float I = (tend + glow) * inner * intensity;
    vec3 c = gen_ramp(TA_TEX0, clamp(k + (1.0 - tend) * 0.15, 0.0, 1.0)).rgb * I;
    return gen_lightPremul(c);
}

// Aura (オーラ強 / 弱): a soft halo hugging the silhouette broken into flickering tongues. Each
// stretch of the edge gets its own tongue length from noise at the turbulence scale; fine streaks
// run along the flow (outward along the edge normals, or straight up with the sides licking up
// too) and the outer half breaks into holes
vec4 gen_auraRising() {
    float len = ta.p[6].x, scale = ta.p[6].y, breakup = ta.p[6].z, tt = ta.p[6].w;
    float intensity = ta.p[7].x, seedN = ta.p[7].y, glowAmount = ta.p[7].z, strong = ta.p[7].w;
    bool up = ta.p[8].x > 0.5;
    bool mobile = ta.p[8].y > 0.5;
    float px = gen_spaceScale();

    vec2 frag = gl_FragCoord.xy;
    float d = gen_sd(frag);
    vec2 pc = gen_spacePos();
    vec2 seed = vec2(seedN * 9.7, seedN * 4.3);
    float dc;           // distance from the silhouette along the flow, canvas px
    vec2 qn, sq, qh;    // tongue-length, streak and hole noise coordinates
    float L = len;
    if (up) {
        // Rising flames: tongues stand taller than outward ones and sway more the higher they get
        L *= 1.6;
        dc = gen_upstream(frag, normalize(gen_dirToTarget(vec2(0.0, 1.0))), L * 1.5 * px, 2.0) / px;
        float xs = pc.x + (aura_fbm(vec2(pc.x / scale, pc.y / (scale * 2.0) - tt * 0.5) + seed, mobile) - 0.5) * dc * 0.8;
        qn = vec2(xs / scale, tt * 0.4);
        sq = vec2(xs / (scale * 0.18), pc.y / (scale * 2.0) - tt);
        qh = vec2(xs / (scale * 0.35), pc.y / (scale * 0.9) - tt);
    } else {
        vec2 nrm = aura_normal();
        dc = max(d, 0.0) / px;
        // Nearest edge point (constant along each normal), bent sideways with distance so the
        // tongues curl as they reach out
        vec2 q = pc - nrm * dc;
        vec2 warp = vec2(aura_fbm(pc / scale + seed, mobile), aura_fbm(pc / scale + seed + 5.1, mobile)) - 0.5;
        vec2 qw = q + warp * dc * 0.8;
        qn = qw / scale + tt * 0.1;
        sq = qw / (scale * 0.18) + tt * 0.3;
        qh = qw / (scale * 0.35) + dc / (scale * 0.9) - tt;
    }
    // fbm sits near 0.5: stretch it so neighbouring tongues differ a lot in length
    float n = clamp((aura_fbm(qn + seed, mobile) - 0.5) * 3.2 + 0.5, 0.0, 1.0);
    float tongueLen = L * (0.2 + 1.3 * n);
    float k = dc / max(tongueLen, 1.0);
    float tongue = pow(clamp(1.0 - k, 0.0, 1.0), 1.4);
    float streak = 0.55 + 0.45 * pow(aura_ridge(sq + seed.yx), 3.0);
    float hole = smoothstep(0.4, 0.65, aura_fbm(qh + seed * 1.7, mobile));
    float brk = 1.0 - breakup * hole * smoothstep(0.05, 0.5, k);
    float halo = exp(-dc / (len * 0.22)) * glowAmount * 0.4;
    float inner = d < 0.0 ? exp(d / (len * 0.08 * px)) : 1.0;
    float I = (tongue * streak * brk + halo) * inner * intensity * mix(0.55, 1.0, strong);
    vec3 c = gen_ramp(TA_TEX0, clamp(k * 0.85 + (1.0 - streak) * 0.1, 0.0, 1.0)).rgb * I;
    return gen_lightPremul(c);
}

#endif // TITLEART_GEN_AURA_GLSL
