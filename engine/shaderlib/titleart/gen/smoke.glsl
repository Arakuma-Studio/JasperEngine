// Smoke, ported from Pachislop src/generators/energy/smoke.ts by SafEight, used with the author's
// permission: soft billowing smoke rising from the edges of the followed layer's content, from
// the upstream distance to its silhouette (tfx_upstream.glsl) taken at a point the billows'
// domain warp moves, with plumes of their own heights and two-tone shading from the same noise.
//
// Drawing space: document space (canvas px, +y up); the field and every upstream position are
// target px. p6 = (flow direction x, y in the drawing space (unit, y up), height, billow size);
// p7 = (phase, seed, side weight, softness); p8 = (highlight linear rgb, 0); p9 = (shadow linear
// rgb, 0). Height and billow size in canvas px. TA_RAW0 the content's distance field.

#ifndef TITLEART_GEN_SMOKE_GLSL
#define TITLEART_GEN_SMOKE_GLSL

#include "titleart/gen/tfx_upstream.glsl"

float smoke_soft3(vec2 p) {
    return 0.55 * vnoise(p) + 0.3 * vnoise(p * 2.07 + 9.1) + 0.15 * vnoise(p * 4.3 + 3.7);
}

vec4 gen_smoke() {
    vec2 dir = ta.p[6].xy;
    float H = ta.p[6].z, W = ta.p[6].w;
    float tt = ta.p[7].x, seedN = ta.p[7].y, side = ta.p[7].z, soft = ta.p[7].w;
    vec3 col = ta.p[8].rgb, col2 = ta.p[9].rgb;
    float px = gen_spaceScale();

    vec2 pc = gen_spacePos();
    vec2 perp = vec2(-dir.y, dir.x);
    float fy = dot(pc, dir), fx = dot(pc, perp);
    vec2 seed = vec2(seedN * 11.7, seedN * 3.9);
    // Billows drift up with the flow and slowly churn (domain warp)
    vec2 q = vec2(fx, fy - tt * W * 1.1) / (W * 2.0) + seed;
    vec2 w = vec2(smoke_soft3(q * 0.6 + vec2(0.0, tt * 0.12)), smoke_soft3(q * 0.6 + vec2(5.2, 1.3 - tt * 0.09))) - 0.5;
    float bil = smoke_soft3(q + w * 1.5);
    // Columns of smoke reach different heights and sway as they rise
    float plume = smoke_soft3(vec2(fx / (W * 2.5), tt * 0.08) + seed.yx);
    vec2 frag = gl_FragCoord.xy + gen_dirToTarget(perp * (w.x * W * 2.0));
    float Hr = H * px;
    float len = Hr * (0.45 + 0.9 * plume);
    float E = gen_upstream(frag, normalize(gen_dirToTarget(dir)), Hr * 1.4, side);
    float I = clamp(1.0 - E / max(len, 1.0), 0.0, 1.0);
    float dens = pow(I, 0.8) * (0.3 + 1.1 * bil);
    dens = smoothstep(0.08, 0.08 + soft, dens);
    vec3 c = mix(col2, col, smoothstep(0.3, 0.75, bil));
    return vec4(c * dens, dens);
}

#endif // TITLEART_GEN_SMOKE_GLSL
