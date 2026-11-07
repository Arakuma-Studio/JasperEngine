// Particles' closed-form animation in QuadBatch's vertex stage (docs/specs/S83-title-effects-
// generators.md §6.7.1 "Animation", D6, R25): each particle's state is a pure function of its
// record and the time, so scrubbing, looping and a live frame need no CPU work and no upload.
// The CPU twin is analysis/ParticleLayout's particleAt (keep the two in step).
//
// A record holds the particle at time 1 (Pachislop's still) and texel 1 .yzw = (the travel that
// brought it there, x and y, its motion phase 0..1). At time t (fract(t + phase) with loop) it
// sits at the end minus the travel not yet made (eased out: fast at birth, at rest at time 1),
// gravity x t^2 lower, turned by spin x t at its own rate (0.5..1.5 from its phase; streaks keep
// their motion's direction), its alpha ramped in and out over its time, and glints flashing
// with its phase. At time 1 with no gravity, spin or fade every value is the record's, exactly.
//
// The block (tv, this program's 16 vec4s): p6.x the shape id; p7 = (time, loop, gravity px,
// spin radians counter-clockwise by time 1); p8 = (twinkle, fade in, fade out, spins 1 / 0).

#ifndef TITLEART_GEN_PARTICLES_MOTION_GLSL
#define TITLEART_GEN_PARTICLES_MOTION_GLSL

const float TFX_MOTION_PI = 3.14159265359;
// whole flashes over time 0..1, so a looping title repeats seamlessly (PARTICLE_TWINKLE_CYCLES)
const float TFX_TWINKLE_CYCLES = 3.0;

void tfx_quadMotion(inout vec4 cs, inout float rot, inout vec4 color, vec3 more) {
    float phase = more.z;
    float time = tv.p[7].x;
    float tau = tv.p[7].y > 0.5 ? fract(time + phase) : time;
    float ease = 1.0 - (1.0 - tau) * (1.0 - tau);
    cs.xy += more.xy * (ease - 1.0) - vec2(0.0, tv.p[7].z * tau * tau);
    if (tv.p[8].w > 0.5) rot += tv.p[7].w * tau * (0.5 + fract(phase * 64.0));
    float f = 1.0;
    if (tv.p[8].y > 0.0) f *= smoothstep(0.0, tv.p[8].y, tau);
    if (tv.p[8].z > 0.0) f *= 1.0 - smoothstep(1.0 - tv.p[8].z, 1.0, tau);
    int shape = int(tv.p[6].x + 0.5);
    if ((shape == 1 || shape == 2) && tv.p[8].x > 0.0) {
        // peaks of 3.2 whose mean over a cycle is 1: a twinkle keeps the field's brightness
        float c = 0.5 + 0.5 * cos(2.0 * TFX_MOTION_PI * (TFX_TWINKLE_CYCLES * time + phase));
        f *= mix(1.0, 3.2 * c * c * c, tv.p[8].x);
    }
    color *= f;
}

#endif // TITLEART_GEN_PARTICLES_MOTION_GLSL
