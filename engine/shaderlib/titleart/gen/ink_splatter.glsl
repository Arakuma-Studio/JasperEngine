// Ink Splatter (墨飛沫), ported from Pachislop src/generators/inkSplatter.ts (blotVert, blotFrag,
// inkFrag) by SafEight, used with the author's permission: each quad is one ink primitive, a
// round cone from a (radius ra) to b (radius rb) with a rim ragged by noise at the drop's own
// scale, antialiased over one pixel of the distance's true slope. Quads draw "over" in the ink
// colour, so coverage unions as Pachislop's white-coverage target coloured afterwards.
//
// A quad is the axis-aligned box of Pachislop's bounding quad (the cone's axis plus room R =
// 1.3 x the larger radius + 1.5 px for the ragged rim and the antialiasing pixel); outside that
// oriented quad nothing is drawn, as there. All lengths in target px with +y up (Pachislop's
// render px): extra = (half of b - a, ra, rb), v_more.x = the drop's noise seed.
// p6 = (ink linear rgb, 0); p7 = (growth 0..1: each primitive scaled about a from nothing, 0, 0, 0).

#ifndef TITLEART_GEN_INK_SPLATTER_GLSL
#define TITLEART_GEN_INK_SPLATTER_GLSL

vec4 gen_inkSplatter() {
    // this pixel from the quad's centre, target px, +y up (the quad is not rotated)
    vec2 P = v_quad * 0.5 * v_sizePx;
    vec2 halfAb = v_extra.xy;
    float ra = v_extra.z, rb = v_extra.w;
    float seed = v_more.x;
    float growth = clamp(ta.p[7].x, 0.0, 1.0);

    // Pachislop's bounding quad around the full-grown primitive: u along a -> b
    vec2 a = -halfAb;
    vec2 fullAb = 2.0 * halfAb;
    float fullLen = length(fullAb);
    vec2 u = fullLen > 1e-3 ? fullAb / fullLen : vec2(1.0, 0.0);
    float R = max(ra, rb) * 1.3 + 1.5;
    float inside = step(abs(dot(P, u)), 0.5 * fullLen + R) * step(abs(dot(P, vec2(-u.y, u.x))), R);

    // growth scales the primitive about its landing point a (1 = as placed)
    vec2 ab = fullAb * growth;
    vec2 b = a + ab;
    float r0 = ra * growth, r1 = rb * growth;
    float t = clamp(dot(P - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    float r = mix(r0, r1, t);
    float d = length(P - (a + ab * t)) - r;
    // ragged rim at the scale of the drop, so big blots wobble and tiny droplets stay round
    float s = max(r1, 1.0);
    float n = vnoise((P - b) / s * 2.2 + seed * 17.0) + 0.5 * vnoise((P - b) / s * 5.0 + seed * 5.0) - 0.75;
    d -= n * 0.45 * r;
    // antialias over one pixel of d's true slope: the ragged rim steepens d to ~2x per px
    // (the derivatives are taken by every pixel, before the masks)
    float cov = clamp(0.5 - d / max(length(vec2(dFdx(d), dFdy(d))), 1e-3), 0.0, 1.0);
    cov *= inside * step(1e-6, growth);
    return vec4(ta.p[6].rgb * cov, cov);
}

#endif // TITLEART_GEN_INK_SPLATTER_GLSL
