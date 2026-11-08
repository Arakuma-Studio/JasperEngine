// Particles (docs/specs/S83-title-effects-generators.md §6.7.1), ported from Pachislop
// src/generators/particles.ts (PARTICLE_FRAG, GLINT_FRAG) by SafEight, used with the author's
// permission: the fragment body of the QuadsParticles draws, one shape per draw (the main shape,
// then the mixed-in glints). The vertex stage moves each quad to the animation time first
// (particles_motion.glsl).
//
// p6 = (shape id, light 1 / solid 0, bokeh ring 0..1, foil sheen 0..1); p7 = (time, loop,
// gravity px, spin radians counter-clockwise by time 1); p8 = (twinkle, fade in, fade out, spins
// 1 / 0). Quad records: extra = (defocus, aspect, tumble phase, glint min half-width in half
// quad heights); texel 1 .yzw = (travel x, y, motion phase).

#ifndef TITLEART_GEN_PARTICLES_GLSL
#define TITLEART_GEN_PARTICLES_GLSL

#include "titleart/gen/tfx_particle_shapes.glsl"

vec4 gen_particles() {
    // the derivatives before any branch on the shape
    vec2 fwq = fwidth(v_quad);
    return tfx_particleShade(int(ta.p[6].x + 0.5), ta.p[6].y > 0.5, v_quad, fwq, v_color, v_extra, ta.p[6].z, ta.p[6].w);
}

#endif // TITLEART_GEN_PARTICLES_GLSL
