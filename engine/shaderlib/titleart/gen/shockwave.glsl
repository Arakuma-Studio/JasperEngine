// Shockwave (衝撃波), ported from Pachislop src/generators/rings/shockwave.ts by SafEight, used with
// the author's permission: concentric light rings with a band profile, soft inner and outer
// edges, fibres, breakup, a radial smear, a chromatic split and a core flash.
//
// p6 = (centre x, y, tilt axis cos, sin); p7 = (squash, radius, spacing, half width);
// p8 = (soft in, soft out, fibre size, ring count); p9 = (profile 0-4, breakup 0-1, breakup
// scale, smear); p10 = (split, flash, seed, 0); p11 = (inner linear rgb, colour span start);
// p12 = (outer linear rgb, colour span end). All in layer px.

#ifndef TITLEART_GEN_SHOCKWAVE_GLSL
#define TITLEART_GEN_SHOCKWAVE_GLSL

const int SW_MAX_RINGS = 8;

// brightness across the band, u = -1 at the inner edge .. +1 at the outer edge
float sw_profileAt(float u, int profile) {
    if (profile == 1) return 1.0 - 0.6 * u * u;
    if (profile == 2) return (0.55 + 0.45 * cos(2.0 * TA_PI * u)) * (1.0 - 0.25 * u * u);
    if (profile == 3) return smoothstep(0.35, 0.65, abs(u));
    return 1.0;
}

// one band edge at signed distance d (> 0 outward) past its half-width with a cubic soft tail
float sw_bandAt(float d, float hIn, float hOut, float sIn, float sOut, float aa, int profile) {
    bool inner = d < 0.0;
    float u = d / (inner ? hIn : hOut);
    float e = abs(d) - (inner ? hIn : hOut);
    float s = inner ? sIn : sOut;
    float cov = clamp(0.5 - e / aa, 0.0, 1.0);
    float tail = e <= 0.0 ? 1.0 : (s > 0.0 ? pow(max(1.0 - e / s, 0.0), 3.0) : 0.0);
    return sw_profileAt(clamp(u, -1.0, 1.0), profile) * max(cov, tail);
}

vec4 gen_shockwave() {
    vec2 center = ta.p[6].xy;
    vec2 axis = ta.p[6].zw;
    float squash = ta.p[7].x, radius = ta.p[7].y, spacing = ta.p[7].z, halfW = ta.p[7].w;
    float softIn = ta.p[8].x, softOut = ta.p[8].y, fibre = ta.p[8].z;
    int count = int(ta.p[8].w);
    int profile = int(ta.p[9].x);
    float breakup = ta.p[9].y, breakScale = ta.p[9].z, smear = ta.p[9].w;
    float split = ta.p[10].x, flash = ta.p[10].y, seed = ta.p[10].z;
    vec3 innerCol = ta.p[11].rgb, outerCol = ta.p[12].rgb;
    vec2 colSpan = vec2(ta.p[11].w, ta.p[12].w);

    vec2 p = gen_spacePos() - center;
    // into the ring's own frame: undo the tilt, then stretch the squashed axis back to a circle
    vec2 q = vec2(axis.x * p.x + axis.y * p.y, -axis.y * p.x + axis.x * p.y);
    q.y /= squash;
    float r = length(q);
    vec2 dir = r > 1e-4 ? q / r : vec2(1.0, 0.0);
    float aa = max(fwidth(r), 1e-4);
    // chromatic split: red drawn split px further out, blue split px further in
    vec3 rc = r - vec3(split, 0.0, -split);

    vec3 rings = vec3(0.0);
    for (int i = 0; i < SW_MAX_RINGS; i++) {
        if (i >= count) break;
        float Ri = radius - float(i) * spacing;
        if (Ri <= 0.0) break;
        float hIn = halfW, hOut = halfW, sIn = softIn, sOut = softOut;
        float gain = pow(0.72, float(i));
        if (profile == 4) {
            // fibres: noise sampled on a circle (seamless), one feature per ~fibre px of circumference
            vec2 fq = dir * (Ri / fibre) + vec2(seed * 7.3 + float(i) * 13.1);
            float f1 = vnoise(fq), f2 = vnoise(fq * 1.7 + 41.0), f3 = vnoise(fq * 3.1 + 77.0);
            hIn *= 0.35 + 1.3 * f1;
            hOut *= 0.5 + f2;
            sIn *= 0.3 + 1.4 * f1;
            sOut *= 0.3 + 1.4 * f2;
            gain *= 0.4 + 0.6 * f3;
        }
        vec3 d = rc - Ri;
        rings += gain * vec3(sw_bandAt(d.r, hIn, hOut, sIn, sOut, aa, profile), sw_bandAt(d.g, hIn, hOut, sIn, sOut, aa, profile),
                             sw_bandAt(d.b, hIn, hOut, sIn, sOut, aa, profile));
    }

    // breakup: gaps across the rings, stretched along them, with a little 2D noise
    if (breakup > 0.0) {
        float n = 0.7 * fbm(dir * (radius / breakScale) + seed * 3.7) + 0.3 * vnoise(q / (0.5 * breakScale) + 19.0);
        float c = mix(0.18, 0.72, breakup);
        rings *= smoothstep(c - 0.12, c + 0.12, n);
    }

    vec3 t = smoothstep(colSpan.x, colSpan.y, rc);
    vec3 light = rings * mix(innerCol, outerCol, t);

    // radial smear: streaks of the ring colour thrown inward and outward, each to its own length
    if (smear > 0.0) {
        vec2 sq = dir * (radius / max(1.5 * fibre, 1.0)) + seed * 3.1 + 211.0;
        float n = 0.6 * vnoise(sq) + 0.4 * vnoise(sq * 2.3 + 5.0);
        n = n * n * n * 2.5;
        float reach = (0.1 + 0.25 * vnoise(sq * 0.7 + 63.0)) * radius + halfW;
        float din = radius - r;
        float fall = din > 0.0 ? exp(-din / (0.22 * radius)) : pow(1.0 - smoothstep(0.0, reach, -din), 2.0);
        fall *= smoothstep(0.0, 0.3 * radius, r);
        light += smear * 0.6 * n * fall * outerCol;
    }

    // core flash in the inner colour: a small white-hot core with a faint halo
    float rr = r / max(radius, 1.0);
    light += flash * (exp(-40.0 * rr * rr) + 0.1 * exp(-6.0 * rr * rr)) * innerCol;
    return gen_lightPremul(light);
}

#endif // TITLEART_GEN_SHOCKWAVE_GLSL
