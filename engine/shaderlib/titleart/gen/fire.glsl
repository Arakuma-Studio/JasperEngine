// Fire, ported from Pachislop src/generators/energy/fire.ts by SafEight, used with the author's
// permission: flames rising from the edges of the followed layer's content, from the upstream
// distance to its silhouette (tfx_upstream.glsl), with a swaying body, rising tongues of their
// own lengths, a fine flicker in the outer part, a white-hot core and, over the edges, flames
// that clear out over the outer band inside the shape.
//
// Drawing space: document space (canvas px, +y up; Pachislop's compPos()); the field and every
// upstream position are target px. p6 = (flow direction x, y in the drawing space (unit, y up),
// height, width); p7 = (turbulence 0-1.5, phase, seed, side weight); p8 = (intensity, over-edge
// depth, over the edges 0/1, 0). Heights, widths and the depth in canvas px. TA_TEX0 the
// gradient (core -> tips), TA_RAW0 the content's distance field.

#ifndef TITLEART_GEN_FIRE_GLSL
#define TITLEART_GEN_FIRE_GLSL

#include "titleart/gen/tfx_upstream.glsl"

vec4 gen_fire() {
    vec2 dir = ta.p[6].xy;
    float H = ta.p[6].z, W = ta.p[6].w;
    float turb = ta.p[7].x, tt = ta.p[7].y, seedN = ta.p[7].z, side = ta.p[7].w;
    float intensity = ta.p[8].x, inside = ta.p[8].y;
    bool overEdge = ta.p[8].z > 0.5;
    float px = gen_spaceScale();

    vec2 pc = gen_spacePos();
    vec2 perp = vec2(-dir.y, dir.x);
    float fy = dot(pc, dir), fx = dot(pc, perp);
    vec2 seed = vec2(seedN * 17.31, seedN * 7.13);
    // Sideways sway of the whole flame body: Pachislop's render-px offset perp x sway x W x turb
    // x 1.4 x uPx is that drawing-space offset carried into target px
    float sway = fbm(vec2(fx / (W * 3.0), fy / (W * 3.5) - tt * 0.7) + seed) - 0.5;
    vec2 frag = gl_FragCoord.xy + gen_dirToTarget(perp * (sway * W * turb * 1.4));
    float Hr = H * px;
    float E = gen_upstream(frag, normalize(gen_dirToTarget(dir)), Hr * 1.6, side);
    // Rising tongue noise: narrow across the flow, stretched along it, scrolling with the flow
    float n = 0.7 * fbm(vec2(fx / W, fy / (W * 4.0) - tt * 1.1) + seed) +
              0.3 * vnoise(vec2(fx / (W * 0.5), fy / (W * 1.6) - tt * 2.0) + seed.yx);
    n = clamp((n - 0.5) * (1.6 + 1.6 * turb) + 0.5, 0.0, 1.0);
    // Each tongue gets its own length; fine flicker roughens only the outer part
    float len = Hr * mix(1.0, 0.3 + 1.3 * n, clamp(turb * 1.3, 0.0, 1.0));
    float f = clamp(1.0 - E / max(len, 1.0), 0.0, 1.0);
    f += (vnoise(vec2(fx / (W * 0.3), fy / (W * 0.6) - tt * 3.0) + seed) - 0.5) * 0.9 * f * (1.0 - f);
    f = clamp(f, 0.0, 1.0);
    float a = smoothstep(0.0, 0.35, f);
    vec3 c = gen_ramp(TA_TEX0, 1.0 - f).rgb;
    c *= 1.0 + 0.6 * smoothstep(0.8, 1.0, f);    // white-hot core
    vec4 fire = vec4(c * intensity * a, a);
    // Over the edge: the flames also lick over the outer band of the shape and clear inside it
    if (overEdge) fire *= 1.0 - smoothstep(0.0, inside * px, max(-gen_sd(gl_FragCoord.xy), 0.0));
    return fire;
}

#endif // TITLEART_GEN_FIRE_GLSL
