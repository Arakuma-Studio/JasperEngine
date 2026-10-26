// The fluid solver of Pavel Dobryakov's WebGL-Fluid-Simulation (MIT, Copyright (c) 2017 Pavel
// Dobryakov; https://github.com/PavelDoGreat/WebGL-Fluid-Simulation): his fragment kernels as
// functions, shared by the native fluid (shaderlib/fluid.frag, FluidSimulator) and the HTML UI
// `fluid` effect. His formulas, constants and boundary rules are kept as they are.
//
// The contract
//
//   Coordinates: uv in 0..1 over the grid the kernel writes, any consistent frame (y up or y
//   down): the solver has no preferred handedness. A texel step `texel` is 1 / grid size. The
//   velocity is in grid texels per second along the same axes (a cell moves uv by
//   velocity * texel * dt), as Pavel's.
//
//   Accessors: the includer defines, before including this file, the reads the kernels it uses
//   need (each a function-like macro or a function of that name), all sampling at uv:
//
//     FLUID_VELOCITY(uv)    -> vec2   the velocity
//     FLUID_CURL(uv)        -> float  the curl written by fluidCurl
//     FLUID_PRESSURE(uv)    -> float  the pressure
//     FLUID_DIVERGENCE(uv)  -> float  the divergence written by fluidDivergence
//     FLUID_SOURCE(uv)      -> vec4   the field advected (fluidAdvect) or splatted (fluidSplat)
//
//   A kernel is defined only when every accessor it reads is (the table below), so a pass
//   defines just its own. The accessors sample with the field's filter: Pavel's linear filter
//   for the velocity and the dye, nearest for the others (the kernels read the latter at texel
//   centres). FLUID_MANUAL_FILTERING (devices without linear filtering of half floats): the
//   advection interpolates four texel-centre reads itself (his bilerp).
//
//     fluidCurl              FLUID_VELOCITY                    -> the curl target (x)
//     fluidVorticity         FLUID_VELOCITY, FLUID_CURL        -> the velocity (xy)
//     fluidDivergence        FLUID_VELOCITY                    -> the divergence target (x)
//     fluidPressureClear     FLUID_PRESSURE                    -> the pressure (x), warm start
//     fluidJacobi            FLUID_PRESSURE, FLUID_DIVERGENCE  -> the pressure (x)
//     fluidGradientSubtract  FLUID_PRESSURE, FLUID_VELOCITY    -> the velocity (xy)
//     fluidAdvect            FLUID_VELOCITY, FLUID_SOURCE      -> the advected field
//     fluidSplat             FLUID_SOURCE                      -> the field with the splat
//
//   One step (Pavel's step(dt)), each kernel a pass into the other image of a ping-pong pair:
//   curl; vorticity (velocity); divergence; pressure clear (x `pressure`); `pressureIterations`
//   Jacobi passes; gradient subtract (velocity); advect the velocity by itself (dissipation
//   `velocityDissipation`, sourceTexel = the velocity's texel); advect the dye (dissipation
//   `densityDissipation`, sourceTexel = the dye's texel). A splat (his splat()) is fluidSplat
//   into the velocity (value = force, 0) then into the dye (value = colour), radius from
//   fluidSplatRadius, aspect = width / height of the fluid.
#ifndef FAM_FLUID_SOLVER_GLSL
#define FAM_FLUID_SOLVER_GLSL

// Pavel's correctRadius(SPLAT_RADIUS / 100): the splat's exp() divisor, widened by the aspect
// on a fluid wider than tall
float fluidSplatRadius(float splatRadius, float aspect){
    float radius = splatRadius / 100.0;
    if (aspect > 1.0)
        radius *= aspect;
    return radius;
}

// the splat's Gaussian at uv: exp(-dot(p, p) / radius), p = uv - point with p.x x aspect
float fluidSplatWeight(vec2 uv, vec2 point, float radius, float aspect){
    vec2 p = uv - point;
    p.x *= aspect;
    return exp(-dot(p, p) / radius);
}

#ifdef FLUID_VELOCITY
// his bilerp of the velocity: four texel-centre reads mixed (FLUID_MANUAL_FILTERING)
vec2 fluidVelocityBilerp(vec2 uv, vec2 tsize){
    vec2 st = uv / tsize - 0.5;
    vec2 iuv = floor(st);
    vec2 fuv = fract(st);
    vec2 a = FLUID_VELOCITY((iuv + vec2(0.5, 0.5)) * tsize);
    vec2 b = FLUID_VELOCITY((iuv + vec2(1.5, 0.5)) * tsize);
    vec2 c = FLUID_VELOCITY((iuv + vec2(0.5, 1.5)) * tsize);
    vec2 d = FLUID_VELOCITY((iuv + vec2(1.5, 1.5)) * tsize);
    return mix(mix(a, b, fuv.x), mix(c, d, fuv.x), fuv.y);
}

// the vorticity: half the curl of the velocity
float fluidCurl(vec2 uv, vec2 texel){
    float L = FLUID_VELOCITY(uv - vec2(texel.x, 0.0)).y;
    float R = FLUID_VELOCITY(uv + vec2(texel.x, 0.0)).y;
    float T = FLUID_VELOCITY(uv + vec2(0.0, texel.y)).x;
    float B = FLUID_VELOCITY(uv - vec2(0.0, texel.y)).x;
    float vorticity = R - L - T + B;
    return 0.5 * vorticity;
}

// the divergence of the velocity, the walls reflecting it (a neighbour outside the grid takes
// the negated velocity of the cell)
float fluidDivergence(vec2 uv, vec2 texel){
    vec2 vL = uv - vec2(texel.x, 0.0);
    vec2 vR = uv + vec2(texel.x, 0.0);
    vec2 vT = uv + vec2(0.0, texel.y);
    vec2 vB = uv - vec2(0.0, texel.y);
    float L = FLUID_VELOCITY(vL).x;
    float R = FLUID_VELOCITY(vR).x;
    float T = FLUID_VELOCITY(vT).y;
    float B = FLUID_VELOCITY(vB).y;
    vec2 C = FLUID_VELOCITY(uv);
    if (vL.x < 0.0) { L = -C.x; }
    if (vR.x > 1.0) { R = -C.x; }
    if (vT.y > 1.0) { T = -C.y; }
    if (vB.y < 0.0) { B = -C.y; }
    return 0.5 * (R - L + T - B);
}
#endif

#if defined(FLUID_VELOCITY) && defined(FLUID_CURL)
// vorticity confinement: the velocity pushed along the gradient of |curl| (x `curl`), clamped
// to +-1000
vec2 fluidVorticity(vec2 uv, vec2 texel, float curl, float dt){
    float L = FLUID_CURL(uv - vec2(texel.x, 0.0));
    float R = FLUID_CURL(uv + vec2(texel.x, 0.0));
    float T = FLUID_CURL(uv + vec2(0.0, texel.y));
    float B = FLUID_CURL(uv - vec2(0.0, texel.y));
    float C = FLUID_CURL(uv);
    vec2 force = 0.5 * vec2(abs(T) - abs(B), abs(R) - abs(L));
    force /= length(force) + 0.0001;
    force *= curl * C;
    force.y *= -1.0;
    vec2 velocity = FLUID_VELOCITY(uv);
    velocity += force * dt;
    velocity = min(max(velocity, -1000.0), 1000.0);
    return velocity;
}
#endif

#ifdef FLUID_PRESSURE
// the warm start: the last pressure x `value` (Pavel's clear pass, his PRESSURE)
float fluidPressureClear(vec2 uv, float value){
    return value * FLUID_PRESSURE(uv);
}
#endif

#if defined(FLUID_PRESSURE) && defined(FLUID_DIVERGENCE)
// one Jacobi relaxation of laplacian(p) = div
float fluidJacobi(vec2 uv, vec2 texel){
    float L = FLUID_PRESSURE(uv - vec2(texel.x, 0.0));
    float R = FLUID_PRESSURE(uv + vec2(texel.x, 0.0));
    float T = FLUID_PRESSURE(uv + vec2(0.0, texel.y));
    float B = FLUID_PRESSURE(uv - vec2(0.0, texel.y));
    float divergence = FLUID_DIVERGENCE(uv);
    return (L + R + B + T - divergence) * 0.25;
}
#endif

#if defined(FLUID_PRESSURE) && defined(FLUID_VELOCITY)
// the projection: the pressure gradient subtracted from the velocity. Pavel's gradient is the
// central difference without its 0.5 (scale 1); 0.5 is the textbook projection
vec2 fluidGradientSubtract(vec2 uv, vec2 texel, float scale){
    float L = FLUID_PRESSURE(uv - vec2(texel.x, 0.0));
    float R = FLUID_PRESSURE(uv + vec2(texel.x, 0.0));
    float T = FLUID_PRESSURE(uv + vec2(0.0, texel.y));
    float B = FLUID_PRESSURE(uv - vec2(0.0, texel.y));
    vec2 velocity = FLUID_VELOCITY(uv);
    velocity.xy -= scale * vec2(R - L, T - B);
    return velocity;
}
#endif

#ifdef FLUID_SOURCE
// his bilerp of the source: four texel-centre reads mixed (FLUID_MANUAL_FILTERING)
vec4 fluidSourceBilerp(vec2 uv, vec2 tsize){
    vec2 st = uv / tsize - 0.5;
    vec2 iuv = floor(st);
    vec2 fuv = fract(st);
    vec4 a = FLUID_SOURCE((iuv + vec2(0.5, 0.5)) * tsize);
    vec4 b = FLUID_SOURCE((iuv + vec2(1.5, 0.5)) * tsize);
    vec4 c = FLUID_SOURCE((iuv + vec2(0.5, 1.5)) * tsize);
    vec4 d = FLUID_SOURCE((iuv + vec2(1.5, 1.5)) * tsize);
    return mix(mix(a, b, fuv.x), mix(c, d, fuv.x), fuv.y);
}

// a splat: the Gaussian x value added to the field's xyz (alpha 1, as his)
vec4 fluidSplat(vec2 uv, vec2 point, vec3 value, float radius, float aspect){
    vec3 splat = fluidSplatWeight(uv, point, radius, aspect) * value;
    vec3 base = FLUID_SOURCE(uv).xyz;
    return vec4(base + splat, 1.0);
}
#endif

#if defined(FLUID_VELOCITY) && defined(FLUID_SOURCE)
// semi-Lagrangian advection: the source read where the velocity came from, faded by
// 1 / (1 + dissipation dt). texel: the velocity grid's; sourceTexel: the source's (manual
// filtering only)
vec4 fluidAdvect(vec2 uv, vec2 texel, vec2 sourceTexel, float dt, float dissipation){
#ifdef FLUID_MANUAL_FILTERING
    vec2 coord = uv - dt * fluidVelocityBilerp(uv, texel) * texel;
    vec4 result = fluidSourceBilerp(coord, sourceTexel);
#else
    vec2 coord = uv - dt * FLUID_VELOCITY(uv) * texel;
    vec4 result = FLUID_SOURCE(coord);
#endif
    float decay = 1.0 + dissipation * dt;
    return result / decay;
}
#endif

#endif // FAM_FLUID_SOLVER_GLSL
