// HTML UI effects (phase 6): the Stable-Fluids family (Jos Stam / Pavel Dobryakov), ported from
// shaders.com's gpu/scaffolds/fluids.ts (buildStableFluidsKernels, buildEmitterSplatKernel,
// buildBrushSplatKernel, buildVelocityImpulseKernel, buildNoiseFieldInitKernel,
// buildTrigTurbulenceKernel, buildNoiseRestoreKernel) and gpu/scaffolds/simShared.ts (the brushes).
//
// The effect declares JFX_STATE(vel) (xy flow, z the curl scratch), JFX_STATE(pd) (x pressure,
// y divergence) and, with a dye, JFX_STATE(dye), all N x N, before including this file. A cell is
// a state texel (x = column, y = row, row 0 at the top of the area): upstream's cell (cx, cy) with
// idx = cy * N + cx. Upstream's in-place compute passes are ping-pong passes here: each function
// returns the new texel of its pass's target from the current images (see phase-6 spec 3.13.1).
// Define FLUID_MASKED and float fluid_mask(ivec2 cell) (1 inside the fluid) before the include for
// the solid-mask variants (SmokeFill).
#ifndef FAM_SIM_FLUID_GLSL
#define FAM_SIM_FLUID_GLSL

#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/fam_paint_noise.glsl"

// simShared.ts: exp(-distSq / radiusSq), and the edge-shifted form (zero at the radius)
float fluid_brush(float distSq, float radiusSq){
    return exp((distSq / radiusSq) * -1.0);
}

float fluid_brush_shifted(float distSq, float radiusSq){
    const float EDGE = 0.36787944117;   // exp(-1)
    return clamp((exp((distSq / radiusSq) * -1.0) - EDGE) * (1.0 / (1.0 - EDGE)), 0.0, 1.0);
}

int fluid_n(){
    return vel_size().x;
}

// the neighbour of a cell under the boundary mode (nidx clamped, nw toroidal)
ivec2 fluid_nb(ivec2 cell, int dx, int dy, bool toroidal){
    int n = fluid_n();
    ivec2 p = cell + ivec2(dx, dy);
    if (toroidal)
        return ivec2((p.x + n) % n, (p.y + n) % n);
    return clamp(p, ivec2(0), ivec2(n - 1));
}

#ifndef FLUID_MASKED
float fluid_mask(ivec2 cell){
    return 1.0;
}
#endif

// ── curl: the z of curl(v), stashed in vel.z ──
vec4 fluid_curl(ivec2 cell, bool toroidal){
    vec4 vL = vel_fetch(fluid_nb(cell, -1, 0, toroidal));
    vec4 vR = vel_fetch(fluid_nb(cell, 1, 0, toroidal));
    vec4 vU = vel_fetch(fluid_nb(cell, 0, -1, toroidal));
    vec4 vD = vel_fetch(fluid_nb(cell, 0, 1, toroidal));
    float curlZ = (vR.y - vL.y - (vD.x - vU.x)) * 0.5;
    vec4 v = vel_fetch(cell);
    return vec4(v.x, v.y, curlZ, 0.0);
}

// ── vorticity confinement (+ the ambient curl-noise breeze of ambientWind, gain baked by the caller) ──
vec4 fluid_vorticity(ivec2 cell, bool toroidal, float curlStrength, float dt, float ambient, float ambientGain, float ambientFreq,
                     float ambientTime){
    float cL = abs(vel_fetch(fluid_nb(cell, -1, 0, toroidal)).z);
    float cR = abs(vel_fetch(fluid_nb(cell, 1, 0, toroidal)).z);
    float cU = abs(vel_fetch(fluid_nb(cell, 0, -1, toroidal)).z);
    float cD = abs(vel_fetch(fluid_nb(cell, 0, 1, toroidal)).z);
    vec4 v = vel_fetch(cell);
    float cc = v.z;
    float gx = (cR - cL) * 0.5;
    float gy = (cD - cU) * 0.5;
    float gl = max(sqrt(gx * gx + gy * gy), 1e-5);
    float fx = gy / gl * cc * curlStrength * dt;
    float fy = (gx / gl) * -1.0 * cc * curlStrength * dt;
    float ax = 0.0;
    float ay = 0.0;
    if (ambient > 0.0){
        float n = float(fluid_n());
        float uvx = (float(cell.x) + 0.5) / n;
        float uvy = (float(cell.y) + 0.5) / n;
        vec2 amb = fam_curl22z(vec2(uvx * ambientFreq, uvy * ambientFreq), ambientTime);
        float af = ambient * ambientGain * dt;
        ax = amb.x * af;
        ay = amb.y * af;
    }
    return vec4(v.x + fx + ax, v.y + fy + ay, v.z, 0.0);
}

// ── divergence + the pressure warm start (into pd: x the decayed pressure, y the divergence) ──
vec4 fluid_divergence(ivec2 cell, bool toroidal, float pressureDecay){
    int n = fluid_n();
    vec4 self = vel_fetch(cell);
    float pressure = pd_fetch(cell).x;
    if (toroidal){
        float vL = vel_fetch(fluid_nb(cell, -1, 0, true)).x;
        float vR = vel_fetch(fluid_nb(cell, 1, 0, true)).x;
        float vU = vel_fetch(fluid_nb(cell, 0, -1, true)).y;
        float vD = vel_fetch(fluid_nb(cell, 0, 1, true)).y;
        float div = (vR - vL + (vD - vU)) * 0.5;
        return vec4(pressure * pressureDecay, div, 0.0, 0.0);
    }
    float jf = float(cell.x);
    float iF = float(cell.y);
#ifdef FLUID_MASKED
    // walls: outside the grid or outside the mask (free-slip reflection of the wall-normal velocity)
    float selfMask = fluid_mask(cell);
    bool isWallL = jf < 0.5 || fluid_mask(fluid_nb(cell, -1, 0, false)) < 0.5;
    bool isWallR = jf > float(n) - 1.5 || fluid_mask(fluid_nb(cell, 1, 0, false)) < 0.5;
    bool isWallT = iF < 0.5 || fluid_mask(fluid_nb(cell, 0, -1, false)) < 0.5;
    bool isWallB = iF > float(n) - 1.5 || fluid_mask(fluid_nb(cell, 0, 1, false)) < 0.5;
    float L = isWallL ? self.x * -1.0 : vel_fetch(fluid_nb(cell, -1, 0, false)).x;
    float R = isWallR ? self.x * -1.0 : vel_fetch(fluid_nb(cell, 1, 0, false)).x;
    float T = isWallT ? self.y * -1.0 : vel_fetch(fluid_nb(cell, 0, -1, false)).y;
    float B = isWallB ? self.y * -1.0 : vel_fetch(fluid_nb(cell, 0, 1, false)).y;
    float div = (R - L + (B - T)) * 0.5 * selfMask;
    return vec4(pressure * pressureDecay * selfMask, div, 0.0, 0.0);
#else
    float L = jf < 0.5 ? self.x * -1.0 : vel_fetch(fluid_nb(cell, -1, 0, false)).x;
    float R = jf > float(n) - 1.5 ? self.x * -1.0 : vel_fetch(fluid_nb(cell, 1, 0, false)).x;
    float T = iF < 0.5 ? self.y * -1.0 : vel_fetch(fluid_nb(cell, 0, -1, false)).y;
    float B = iF > float(n) - 1.5 ? self.y * -1.0 : vel_fetch(fluid_nb(cell, 0, 1, false)).y;
    float div = (R - L + (B - T)) * 0.5;
    return vec4(pressure * pressureDecay, div, 0.0, 0.0);
#endif
}

// ── one Jacobi relaxation of laplacian(p) = div(v) ──
vec4 fluid_jacobi(ivec2 cell, bool toroidal){
    float pL = pd_fetch(fluid_nb(cell, -1, 0, toroidal)).x;
    float pR = pd_fetch(fluid_nb(cell, 1, 0, toroidal)).x;
    float pU = pd_fetch(fluid_nb(cell, 0, -1, toroidal)).x;
    float pD = pd_fetch(fluid_nb(cell, 0, 1, toroidal)).x;
    float div = pd_fetch(cell).y;
    return vec4((pL + pR + pU + pD - div) * 0.25, div, 0.0, 0.0);
}

// ── project: v -= grad(p) (with the optional velocity cap, 0: none) ──
vec4 fluid_project(ivec2 cell, bool toroidal, float velocityCap){
    float pL = pd_fetch(fluid_nb(cell, -1, 0, toroidal)).x;
    float pR = pd_fetch(fluid_nb(cell, 1, 0, toroidal)).x;
    float pU = pd_fetch(fluid_nb(cell, 0, -1, toroidal)).x;
    float pD = pd_fetch(fluid_nb(cell, 0, 1, toroidal)).x;
    vec4 v = vel_fetch(cell);
    float vx = v.x - (pR - pL) * 0.5;
    float vy = v.y - (pD - pU) * 0.5;
    if (velocityCap > 0.0){
        float vLen = max(sqrt(vx * vx + vy * vy), 1e-6);
        float capScale = velocityCap / max(vLen, velocityCap);
        return vec4(vx * capScale, vy * capScale, v.z, 0.0);
    }
    return vec4(vx, vy, v.z, 0.0);
}

// the bilinear backtrace of upstream's advect kernels: the four cells and the weights
struct FluidTrace {
    ivec2 c00, c10, c01, c11;
    float sx, sy;
};

FluidTrace fluid_trace(ivec2 cell, vec2 velocity, float dt, bool toroidal){
    float n = float(fluid_n());
    FluidTrace t;
    if (toroidal){
        // WGSL's % on floats is the truncated remainder (the operand is positive here)
        float ax = float(cell.x) - velocity.x * dt + n;
        float ay = float(cell.y) - velocity.y * dt + n;
        float srcX = ax - n * trunc(ax / n);
        float srcY = ay - n * trunc(ay / n);
        float x0 = floor(srcX);
        float y0 = floor(srcY);
        t.sx = srcX - x0;
        t.sy = srcY - y0;
        int ix0 = int(x0);
        int iy0 = int(y0);
        int nn = fluid_n();
        int ix1 = (ix0 + 1) % nn;
        int iy1 = (iy0 + 1) % nn;
        t.c00 = ivec2(ix0, iy0);
        t.c10 = ivec2(ix1, iy0);
        t.c01 = ivec2(ix0, iy1);
        t.c11 = ivec2(ix1, iy1);
        return t;
    }
    float srcX = clamp(float(cell.x) - velocity.x * dt, 0.0, n - 1.0);
    float srcY = clamp(float(cell.y) - velocity.y * dt, 0.0, n - 1.0);
    float x0 = clamp(floor(srcX), 0.0, n - 2.0);
    float y0 = clamp(floor(srcY), 0.0, n - 2.0);
    t.sx = srcX - x0;
    t.sy = srcY - y0;
    int ix0 = int(x0);
    int iy0 = int(y0);
    t.c00 = ivec2(ix0, iy0);
    t.c10 = ivec2(ix0 + 1, iy0);
    t.c01 = ivec2(ix0, iy0 + 1);
    t.c11 = ivec2(ix0 + 1, iy0 + 1);
    return t;
}

// ── semi-Lagrangian advection of the velocity (copyVel's z = 0 folded in) ──
vec4 fluid_advect_vel(ivec2 cell, bool toroidal, float dt, float velFade){
    vec4 v = vel_fetch(cell);
    FluidTrace t = fluid_trace(cell, v.xy, dt, toroidal);
    vec4 v00 = vel_fetch(t.c00);
    vec4 v10 = vel_fetch(t.c10);
    vec4 v01 = vel_fetch(t.c01);
    vec4 v11 = vel_fetch(t.c11);
    float decay = 1.0 + velFade * dt;
    float m = fluid_mask(cell);
    return vec4(mix(mix(v00.x, v10.x, t.sx), mix(v01.x, v11.x, t.sx), t.sy) / decay * m,
                mix(mix(v00.y, v10.y, t.sx), mix(v01.y, v11.y, t.sx), t.sy) / decay * m, 0.0, 0.0);
}

// ── the dye's advection: densityAge (x density, y age) with dissipation and ageing, or permanent ──
vec4 fluid_advect_density_age(ivec2 cell, bool toroidal, float dt, bool dissipateAndAge, float dyeFade, float colorDecay){
    vec4 v = vel_fetch(cell);
    FluidTrace t = fluid_trace(cell, v.xy, dt, toroidal);
    vec4 d00 = dye_fetch(t.c00);
    vec4 d10 = dye_fetch(t.c10);
    vec4 d01 = dye_fetch(t.c01);
    vec4 d11 = dye_fetch(t.c11);
    float dens = mix(mix(d00.x, d10.x, t.sx), mix(d01.x, d11.x, t.sx), t.sy);
    float age = mix(mix(d00.y, d10.y, t.sx), mix(d01.y, d11.y, t.sx), t.sy);
    if (!dissipateAndAge)
        return vec4(dens, age, 0.0, 0.0);
    float decay = 1.0 + dyeFade * dt;
    return vec4(max(dens / decay, 0.0) * fluid_mask(cell), min(age + dt * 0.4 * colorDecay, 1.0), 0.0, 0.0);
}

// ── the RGB dye's advection with dissipation (InkFlow) ──
vec4 fluid_advect_rgb(ivec2 cell, float dt, float dyeFade){
    vec4 v = vel_fetch(cell);
    FluidTrace t = fluid_trace(cell, v.xy, dt, false);
    vec4 c00 = dye_fetch(t.c00);
    vec4 c10 = dye_fetch(t.c10);
    vec4 c01 = dye_fetch(t.c01);
    vec4 c11 = dye_fetch(t.c11);
    vec3 c = mix(mix(c00.xyz, c10.xyz, t.sx), mix(c01.xyz, c11.xyz, t.sx), t.sy);
    float decay = 1.0 + dyeFade * dt;
    return vec4(max(c / decay, vec3(0.0)), 0.0);
}

// ── the cone emitter of Smoke and SmokeFill (cone + cursorPush [+ masked]), in grid cells ──
struct FluidEmitter {
    float dt;
    vec2 emit;          // the source, cells
    vec2 emitVel;       // the injected velocity, cells/s
    vec2 perpDir;
    float spreadFactor;
    float emitRad;      // cells
    float emitIntensity;
    float gravity;
    vec2 cursor;        // cells
    vec2 cursorVel;
    float mouseActive;
    float mouseRadSq;
    float densityGain;
    float velocityBlendGain;
};

float fluid_emitter_influence(ivec2 cell, FluidEmitter e, out float dx, out float dy){
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    dx = cellX - e.emit.x;
    dy = cellY - e.emit.y;
    float distSq = dx * dx + dy * dy;
    float radSq = max(e.emitRad * e.emitRad, 1.0);
    return fluid_brush(distSq, radSq) * fluid_mask(cell);
}

// the dye of the cone emitter (density added, age rejuvenated)
vec4 fluid_emit_dye(ivec2 cell, FluidEmitter e, vec4 dyeCell){
    float dx, dy;
    float influence = fluid_emitter_influence(cell, e, dx, dy);
    float oldDens = dyeCell.x;
    float addDens = e.emitIntensity * influence * e.dt * e.densityGain;
    float newDens = min(oldDens + addDens, 1.0);
    float rejuvenate = addDens / max(newDens, 0.001);
    return vec4(newDens, dyeCell.y * (1.0 - min(rejuvenate, 1.0)), 0.0, 0.0);
}

// the velocity of the cone emitter: the blend toward the fanned injection, gravity on the old
// density, the cursor shove
vec4 fluid_emit_vel(ivec2 cell, FluidEmitter e, vec4 v, float oldDens){
    float dx, dy;
    float influence = fluid_emitter_influence(cell, e, dx, dy);
    float m = fluid_mask(cell);
    float n = float(fluid_n());
    float perpOff = (dx * e.perpDir.x + dy * e.perpDir.y) / max(e.emitRad, 1.0);
    float vmSq = max(e.emitVel.x * e.emitVel.x + e.emitVel.y * e.emitVel.y, 1.0);
    float tvx = e.emitVel.x + e.perpDir.x * perpOff * e.spreadFactor * sqrt(vmSq);
    float tvy = e.emitVel.y + e.perpDir.y * perpOff * e.spreadFactor * sqrt(vmSq);
    float br = min(influence * e.dt * e.velocityBlendGain, 1.0);
    float gravForce = e.gravity * oldDens * e.dt * (n * 0.08) * m;
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    float cdx = cellX - e.cursor.x;
    float cdy = cellY - e.cursor.y;
    float cursorInf = fluid_brush(cdx * cdx + cdy * cdy, max(e.mouseRadSq, 1.0)) * e.mouseActive * m;
    return vec4(mix(v.x, tvx, br) + e.cursorVel.x * cursorInf, mix(v.y, tvy, br) + gravForce + e.cursorVel.y * cursorInf, v.z, 0.0);
}

// ── the gated puff (SmokeFlow: the source is the cursor, emission x emitGate), one stamp ──
vec4 fluid_puff_dye(ivec2 cell, vec2 emit, float emitRad, float emitGate, float emitIntensity, float dt, float densityGain, vec4 dyeCell){
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    float dx = cellX - emit.x;
    float dy = cellY - emit.y;
    float influence = fluid_brush(dx * dx + dy * dy, max(emitRad * emitRad, 1.0)) * emitGate;
    float oldDens = dyeCell.x;
    float addDens = emitIntensity * influence * dt * densityGain;
    float newDens = min(oldDens + addDens, 1.0);
    float rejuvenate = addDens / max(newDens, 0.001);
    return vec4(newDens, dyeCell.y * (1.0 - min(rejuvenate, 1.0)), 0.0, 0.0);
}

vec4 fluid_puff_vel(ivec2 cell, vec2 emit, vec2 emitVel, float emitRad, float emitGate, float dt, float velocityBlendGain, float gravity,
                    float oldDens, vec4 v){
    float n = float(fluid_n());
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    float dx = cellX - emit.x;
    float dy = cellY - emit.y;
    float influence = fluid_brush(dx * dx + dy * dy, max(emitRad * emitRad, 1.0)) * emitGate;
    float br = min(influence * dt * velocityBlendGain, 1.0);
    float gravForce = gravity * oldDens * dt * (n * 0.08);
    return vec4(mix(v.x, emitVel.x, br), mix(v.y, emitVel.y, br) + gravForce, v.z, 0.0);
}

// ── the velocity impulse (ParticleFlow's force stamp): added momentum ──
vec4 fluid_impulse(ivec2 cell, vec2 pos, vec2 velocity, float radius, vec4 v){
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    float dx = cellX - pos.x;
    float dy = cellY - pos.y;
    float inf = fluid_brush(dx * dx + dy * dy, max(radius * radius, 1.0));
    return vec4(v.x + velocity.x * inf, v.y + velocity.y * inf, v.z, 0.0);
}

// ── the brush (InkFlow): the colour painted over the dye (a mix), and the added impulse ──
vec4 fluid_brush_dye(ivec2 cell, vec2 pos, float radius, float strength, vec3 brush, vec4 dyeCell){
    float cellX = float(cell.x) + 0.5;
    float cellY = float(cell.y) + 0.5;
    float dx = cellX - pos.x;
    float dy = cellY - pos.y;
    float inf = fluid_brush(dx * dx + dy * dy, max(radius * radius, 1.0));
    float w = clamp(inf * strength, 0.0, 1.0);
    return vec4(mix(dyeCell.x, brush.x, w), mix(dyeCell.y, brush.y, w), mix(dyeCell.z, brush.z, w), 0.0);
}

// ── stamp ribbons (pathStampRibbon): numSteps = min(maxSteps, max(1, ceil(dragDist / stepSize))),
//    stamp s at t = (s + 0.5) / numSteps along the drag (uv) ──
int fluid_ribbon_steps(float dragDist, float stepSize, int maxSteps){
    return min(maxSteps, max(1, int(ceil(dragDist / stepSize))));
}

// ── seeded noise fields (Fog): a 3-octave value-noise fBm offset by a seed-hashed jump ──
float fluid_value_hash(float px, float py){
    return jfx_hash12(vec2(px, py));
}

float fluid_value_noise(float nx, float ny){
    float ix = floor(nx);
    float iy = floor(ny);
    float sx = fract(nx);
    float sy = fract(ny);
    float ux = sx * sx * (3.0 - sx * 2.0);
    float uy = sy * sy * (3.0 - sy * 2.0);
    return mix(mix(fluid_value_hash(ix, iy), fluid_value_hash(ix + 1.0, iy), ux),
               mix(fluid_value_hash(ix, iy + 1.0), fluid_value_hash(ix + 1.0, iy + 1.0), ux), uy);
}

float fluid_fbm(float nx, float ny){
    return fluid_value_noise(nx, ny) * 0.5 + fluid_value_noise(nx * 2.0 + 5.37, ny * 2.0 + 9.13) * 0.3 +
           fluid_value_noise(nx * 4.0 + 1.79, ny * 4.0 + 3.51) * 0.2;
}

vec2 fluid_seed_lattice(float x, float y, float seed, float cellsPerTexel){
    float sdx = jfx_hash11(seed * 37.31) * 97.0;
    float sdy = jfx_hash11(seed * 83.17) * 97.0;
    return vec2(x * cellsPerTexel + sdx, y * cellsPerTexel + sdy);
}

#endif // FAM_SIM_FLUID_GLSL
