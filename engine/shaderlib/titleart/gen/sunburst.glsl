// Sunburst (放射 / 集中線), ported from Pachislop src/generators/lines/sunburst.ts (frag,
// burstFrag) by SafEight, used with the author's permission: rays from a centre over the whole
// frame, one fullscreen pass that writes every pixel (opaque: a background). Positions are layer
// px; angles are Pachislop's (radians counter-clockwise, +y up). TA_TEX0 is the gradient ramp,
// TA_TEX1 the followed layer before its effects (On the Layer: clip, over).
//
// p11 = (style: 0 wedges, 1 focus lines, 2 speed rings, 3 sunburst; 1 when TA_TEX1 holds the layer).
// Sunburst: p6 = (centre x, y, rays, twist per layer px); p7 = (rotation, inner radius, softness
// 0..1, centre glow); p8 = (gradient radius: the farthest frame corner, duty 0..1, 1 = coloured by
// the gradient along the radius, 1 = only on the layer's pixels); p9 = colour A, p10 = colour B
// (linear rgb).
// Bursts: p6 = (centre x, y, rays, seed); p7 = (rotation, inner radius, inner softness 0..1,
// opacity); p8 = (width variation, length variation, duty, on the layer: 0 rays only, 1 on its
// pixels only, 2 on them and around); p9 = colour A, p10 = (colour B, 1 = two colours).

#ifndef TITLEART_GEN_SUNBURST_GLSL
#define TITLEART_GEN_SUNBURST_GLSL

const float SB_TAU = 6.2831853;

float sb_toLin1(float x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

// Pachislop's sRGB encode clamps to 0..1 (ta_toSrgb only clamps below)
vec3 sb_toSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec4 sb_sunburst() {
    vec2 center = ta.p[6].xy;
    float rays = ta.p[6].z, twist = ta.p[6].w;
    float rot = ta.p[7].x, inner = ta.p[7].y, soft = ta.p[7].z, glow = ta.p[7].w;
    float rc = ta.p[8].x, duty = ta.p[8].y;
    bool radial = ta.p[8].z > 0.5, clip = ta.p[8].w > 0.5;
    vec3 colA = ta.p[9].rgb, colB = ta.p[10].rgb;

    vec2 d = gen_spacePos() - center;
    float r = length(d);
    float k = gen_spaceScale();
    float th = atan(d.y, d.x) - rot + twist * r;
    float u = fract(th / SB_TAU * rays + 0.5) - 0.5;  // -0.5..0.5 across one ray period, ray = |u| < duty / 2
    // one period's arc in target px, shortened by the slant of twisted edges
    float period = SB_TAU * r * k / rays / sqrt(1.0 + r * r * twist * twist);
    float edge = mix(1.0, 0.5 * period, soft);
    float cov = smoothstep(0.0, 1.0, clamp((0.5 * duty - abs(u)) * period / max(edge, 1e-3) + 0.5, 0.0, 1.0));
    // rays thinner than a few pixels fade to their average: the hub never moirés
    cov = mix(0.5, cov, clamp(0.5 * period - 1.0, 0.0, 1.0));
    // inside the inner radius the rays merge into a disc of the ray colour
    float disc = clamp((inner - r) * k / max(1.0, soft * inner * k * 0.6) + 0.5, 0.0, 1.0);
    cov = max(cov, disc);
    vec3 ray = colA;
    if (radial) {
        vec4 g = gen_ramp(TA_TEX0, clamp(r / rc, 0.0, 1.0));
        ray = mix(colB, g.rgb, g.a);
    }
    vec3 c = mix(colB, ray, cov);
    // the centre glow: a whitened ray colour, about twice the inner disc wide
    float gr = max(inner * 1.6, 0.15 * rc);
    vec3 hot = mix(radial ? gen_ramp(TA_TEX0, 0.0).rgb : colA, vec3(1.0), 0.6);
    c += hot * glow * exp(-2.0 * r * r / (gr * gr));
    return clip ? vec4(c, 1.0) * texture(TA_TEX1, ta_uv()).a : vec4(c, 1.0);
}

vec4 sb_bursts(int style, bool hasSrc) {
    vec2 center = ta.p[6].xy;
    float n = ta.p[6].z, seed = ta.p[6].w;
    float rot = ta.p[7].x, inner = ta.p[7].y, soft = ta.p[7].z, opacity = ta.p[7].w;
    float variation = ta.p[8].x, lenVar = ta.p[8].y, duty = ta.p[8].z;
    int mode = int(ta.p[8].w + 0.5);
    vec3 col1 = ta.p[9].rgb, col2 = ta.p[10].rgb;
    bool alternate = ta.p[10].w > 0.5;

    vec4 s = hasSrc ? texture(TA_TEX1, ta_uv()) : vec4(0.0);
    vec2 d = gen_spacePos() - center;
    float r = length(d);
    float ang = atan(d.y, d.x) - rot;
    float aa = 1.0 / gen_spaceScale();     // one target pixel in layer px
    float cov = 0.0;                        // ray coverage 0..1
    float k = 0.0;                          // 0/1 colour select for alternating colours
    if (style < 2) {
        // the sector around the centre; each sector holds one ray
        float u = fract(ang / SB_TAU) * n;
        float i = floor(u);
        if (i >= n) i = 0.0;
        float f = u - i;                    // 0..1 across the sector
        float h1 = hash12(vec2(i, seed)), h2 = hash12(vec2(i + 31.0, seed)), h3 = hash12(vec2(i + 77.0, seed));
        float sectorPx = SB_TAU * r / n;    // the sector's arc at this radius, layer px
        if (style == 0) {
            // wedges: the angular width varies per ray, edges anti-aliased in pixels
            float w = duty * (1.0 - variation * h1);
            float c0 = 0.5 + (h2 - 0.5) * (1.0 - w) * variation;
            cov = clamp((0.5 * w - abs(f - c0)) * sectorPx / aa + 0.5, 0.0, 1.0);
        } else {
            // focus lines: needle-shaped rays starting at random radii beyond the inner radius
            float start = inner * (1.0 + lenVar * 1.5 * h3);
            float grow = max(inner * 0.6, 40.0);
            float w = duty * (1.0 - variation * h1) * smoothstep(start, start + grow, r);
            float c0 = 0.5 + (h2 - 0.5) * 0.7;
            cov = clamp((0.5 * w - abs(f - c0)) * sectorPx / aa + 0.5, 0.0, 1.0) * step(start, r);
            k = step(0.5, h3);
        }
    } else {
        // concentric speed rings: broken arcs at regular radii (rays = rings per 1,000 canvas px,
        // so the pattern does not depend on the canvas size)
        float spacing = 1000.0 / n;
        float j = floor(r / spacing);
        float fr = r / spacing - j;
        float h1 = hash12(vec2(j, seed)), h2 = hash12(vec2(j + 13.0, seed)), h3 = hash12(vec2(j + 57.0, seed));
        float w = duty * (1.0 - variation * h1);
        float band = clamp((0.5 * w - abs(fr - 0.5)) * spacing / aa + 0.5, 0.0, 1.0);
        float segs = floor(3.0 + 9.0 * h2);
        float sa = fract(ang / SB_TAU * segs + h3);
        float arcLen = mix(1.0, 0.35 + 0.5 * h1, lenVar);
        float arcPx = SB_TAU * r / segs;
        float arc = clamp((0.5 * arcLen - abs(sa - 0.5)) * arcPx / aa + 0.5, 0.0, 1.0);
        cov = band * arc;
        k = mod(j, 2.0);
    }
    // the centre cleared with a soft edge
    float clear = smoothstep(inner * (1.0 - soft) - aa, inner, r);
    vec3 c;
    float a;
    if (style == 0 && alternate) {
        // two-colour wedges: colour A wedges on a colour B ground
        c = mix(col2, col1, cov);
        a = clear * opacity;
    } else {
        c = alternate && k > 0.5 ? col2 : col1;
        a = cov * clear * opacity;
    }
    // the ray opacity is perceptual (Photoshop-like, coverage in sRGB): over the followed layer's
    // own pixels the rays blend in sRGB, its energy above 1 kept
    vec3 sb = ta_unpremul(s);
    vec3 mixed = ta_toLinear(mix(sb_toSrgb(sb), sb_toSrgb(c), a)) + max(sb - 1.0, 0.0) * (1.0 - a);
    // elsewhere the layer composites in linear light later: the alpha that gives the same
    // brightness over black (dark rays keep the plain alpha)
    float ls = dot(sb_toSrgb(c), vec3(0.2126, 0.7152, 0.0722));
    float aT = ls > 1e-4 ? clamp(sb_toLin1(a * ls) / sb_toLin1(ls), 0.0, 1.0) : a;
    vec4 rayT = vec4(c * aT, aT);
    vec4 onLayer = vec4(mixed * s.a, s.a);
    return mode == 1 ? onLayer : mode == 2 ? onLayer + rayT * (1.0 - s.a) : rayT;
}

vec4 gen_sunburst() {
    int style = int(ta.p[11].x + 0.5);
    return style == 3 ? sb_sunburst() : sb_bursts(style, ta.p[11].y > 0.5);
}

#endif // TITLEART_GEN_SUNBURST_GLSL
