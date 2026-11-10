// The energy generators' "upstream" distance (docs/specs/S83-title-effects-generators.md R24),
// ported from Pachislop src/generators/energy/shared.ts (SD_GLSL upstream) by SafEight, used with
// the author's permission: how far the followed layer's edge is when looking back against the
// flow, with the sideways distance weighted by `side` so flames also lick up the sides. Built on
// gen_sd (tfx_field.glsl: the content's distance field in TA_RAW0, target px), so every
// position, direction and length here is in target px (rows down): callers turn Pachislop's
// y-up flow direction into a target-px unit vector first (gen_dirToTarget, normalized).

#ifndef TITLEART_GEN_TFX_UPSTREAM_GLSL
#define TITLEART_GEN_TFX_UPSTREAM_GLSL

#include "titleart/gen/tfx_field.glsl"

// the smallest of max(sd(frag - dir x st), 0) x side + st over 40 steps st of 0..len
float gen_upstream(vec2 frag, vec2 dir, float len, float side) {
    float E = 1e9;
    for (int i = 0; i < 40; i++) {
        float st = len * float(i) / 39.0;
        if (st > E) break;
        float d = max(gen_sd(frag - dir * st), 0.0);
        E = min(E, d * side + st);
    }
    return E;
}

#endif // TITLEART_GEN_TFX_UPSTREAM_GLSL
