#version 450
// @effect     particles
// @kind       paint
// @category   interactive
// @summary    A 3D swarm that settles evenly into a sphere, scattering from the pointer and drifting back
// @source     shaders.com Particles (adapted: the default sphere container, centred, scale 1)
// @cost       stateful
// @pointer    trail
// @state      pos rgba32f 128x128 nearest
// @state      vel rgba32f 128x128 nearest
// @state      dplace rgba32f 1024x128 nearest
// @state      dens rgba16f 256x128 nearest
// @state      place rgba32f 128x128 nearest
// @state      accum rgba16f box max 1024 nearest
// @state      field rgba16f box max 1024 linear
// @init       spawn pos
// @step       dplace dplace
// @step       density dens scatter dplace
// @step       forces vel
// @step       move pos
// @step       place place
// @step       splat accum scatter place
// @step       resolve field
// @suggest-for swarm particle-swarm
// @param      count integer 4000 1000 16000 -- Number of particles
// @param      size number 1.5 0.6 6 -- Base particle size (px; each varies, and grows toward the viewer)
// @param      particle-shape enum dot dot|square|glow -- What a particle is drawn as
// @param      spread number 1 0.1 3 -- The pressure pushing particles apart (how evenly they fill the sphere)
// @param      agitation number 0.12 0 1 -- Idle thermal motion (0 freezes the settled swarm)
// @param      damping number 0.4 0 1 -- How quickly disturbed particles calm down
// @param      gravity number 0 -2 2 -- Downward pull on the swarm (negative floats up)
// @param      mouse-influence number 2 -5 5 -- The pointer's field: positive scatters, negative pulls the swarm in
// @param      mouse-radius number 0.2 0.05 0.8 -- Reach of the pointer's field (shape units: the sphere is 0.7 across)
// @param      exposure number 1 0.2 3 -- Brightness of the additive glow
// @param      softness number 0.1 0 1 -- Edge softness of each particle (0 a crisp disc)
// @param      color-a color #8ec5ff -- Colour of particles at rest
// @param      color-b color #ff7ad9 -- Colour particles shift toward when agitated
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the rest-to-excited ramp

// Ported from shaders.com Particles (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/Particles/index.ts, std/sim/agentForces.ts pressure/containment/cursorXY/gravity/
// turbulence/forces3d, gpu/kit/agents.ts makeBallCloudInit, std/sim/agentRender.ts volume splat,
// gpu/kit/sdf3d.ts sdSphere): a true 3D swarm in shape-local space that spreads by descending a
// 32^3 density grid (re-splatted every step, dithered by the R3 sub-cell offset), held in its shape
// by the containment bundle, pushed by the pointer, gravity and a hash turbulence, projected with
// a soft perspective and coloured by speed. The shape is the default preset (sphere3D, radius 0.35);
// its centre, scale, rotation and shape props are not ported (phase 5's shape rule). The density
// grid is a 256 x 128 atlas of 32 x 32 slices; its trilinear splat is 8 one-texel quads a particle.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"
#include "effects/lib/fam_sim_agents.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float count;
    float size;
    float particleShape;
    float spread;
    float agitation;
    float damping;
    float gravity;
    float mouseInfluence;
    float mouseRadius;
    float exposure;
    float softness;
    float colorSpace;
} params;

JFX_STATE(pos)
JFX_STATE(vel)
JFX_STATE(dplace)
JFX_STATE(dens)
JFX_STATE(place)
JFX_STATE(accum)
JFX_STATE(field)

const int MAX_PARTICLES = 16000;
const int TABLE = 128;
const float DGRID = 32.0;
const float DOMAIN = 1.0;
const float EXTQ = 2.6;
const float MAX_SPLAT_SIZE = 6.0;
const float MAX_SPEED = 3.0;
const vec3 R3_ALPHA = vec3(0.8191725133961645, 0.6710436067037893, 0.5497004779019703);

vec4 jfx_paint(JfxContext c){
    return field_sample(c.uv);
}

#ifdef JFX_STEP_FORM
float swarm_count(){
    return clamp(floor(params.count + 0.5), 16.0, float(MAX_PARTICLES));
}

float swarm_dt(JfxStep s){
    return min(max(s.dt, 0.001), 0.033);
}

// the R3 low-discrepancy dither of the density grid, per step
vec3 swarm_grid_offset(JfxStep s){
    float frameIdx = max(s.steps - 1.0, 0.0);
    float cell = (2.0 * DOMAIN) / DGRID;
    return fract(frameIdx * R3_ALPHA) * cell;
}

// a density cell's atlas texel (8 x 4 slices of 32 x 32)
ivec2 swarm_atlas(int x, int y, int z){
    return ivec2((z % 8) * 32 + x, (z / 8) * 32 + y);
}

float swarm_dens_at(int cx, int cy, int cz){
    int g = int(DGRID);
    return dens_fetch(swarm_atlas(clamp(cx, 0, g - 1), clamp(cy, 0, g - 1), clamp(cz, 0, g - 1))).x * (1.0 / 64.0);
}

// the particle's position in fractional grid cells, its base corner and fraction
void swarm_grid(vec3 p, vec3 gridOff, out ivec3 base, out vec3 frac){
    float g = DGRID;
    vec3 c = (p + gridOff + DOMAIN) / (2.0 * DOMAIN) * g - 0.5;
    vec3 b = clamp(floor(c), vec3(0.0), vec3(g - 2.0));
    frac = clamp(c - b, vec3(0.0), vec3(1.0));
    base = ivec3(b);
}

// the trilinear density and its gradient (densTrilinearGrad): (d/dx, d/dy, d/dz, value)
vec4 swarm_dens_grad(vec3 p, vec3 gridOff){
    ivec3 base;
    vec3 f;
    swarm_grid(p, gridOff, base, f);
    float acc = 0.0, gx = 0.0, gy = 0.0, gz = 0.0;
    for (int dz = 0; dz < 2; dz++){
        for (int dy = 0; dy < 2; dy++){
            for (int dx = 0; dx < 2; dx++){
                float wx = mix(1.0 - f.x, f.x, float(dx));
                float wy = mix(1.0 - f.y, f.y, float(dy));
                float wz = mix(1.0 - f.z, f.z, float(dz));
                float sx = float(dx) * 2.0 - 1.0;
                float sy = float(dy) * 2.0 - 1.0;
                float sz = float(dz) * 2.0 - 1.0;
                float cv = swarm_dens_at(base.x + dx, base.y + dy, base.z + dz);
                acc += cv * (wx * wy * wz);
                gx += cv * (sx * wy * wz);
                gy += cv * (wx * sy * wz);
                gz += cv * (wx * wy * sz);
            }
        }
    }
    return vec4(gx, gy, gz, acc);
}

float swarm_field(vec3 p){
    return length(p) - 0.35;   // sdSphere, the sphere3D preset
}

// the cursor in shape-local xy (y up): pointerToShapeLocal with the centre (0.5, 0.5), scale 1
vec2 swarm_cursor(JfxStep s){
    vec2 size = max(s.c.size, vec2(1.0));
    float aspect = size.x / size.y;
    vec2 p = s.pointer / size;
    return vec2((p.x - 0.5) * aspect, -(p.y - 0.5));
}

vec3 swarm_force(vec3 p, vec3 v, float seed, JfxStep s){
    // pressure
    vec4 gv = swarm_dens_grad(p, swarm_grid_offset(s));
    float pressNorm = params.spread * 4.4 / max(gv.w * 2.0 + 6.0, 6.0);
    vec3 F = -gv.xyz * pressNorm;
    // containment (gradEps 0.02; the rotation entrainment is 0: the shape does not turn)
    float sd = swarm_field(p);
    float e = 0.02;
    vec3 g = vec3(swarm_field(p + vec3(e, 0.0, 0.0)) - swarm_field(p - vec3(e, 0.0, 0.0)),
                  swarm_field(p + vec3(0.0, e, 0.0)) - swarm_field(p - vec3(0.0, e, 0.0)),
                  swarm_field(p + vec3(0.0, 0.0, e)) - swarm_field(p - vec3(0.0, 0.0, e)));
    vec3 gn = g / max(length(g), 1e-4);
    float wallF = smoothstep(-0.10, 0.01, sd) * 1.1 + max(sd, 0.0) * 22.0;
    F -= gn * wallF;
    float rl = length(p);
    F -= p * (smoothstep(DOMAIN * 0.85, DOMAIN * 1.1, rl) * 9.0 / max(rl, 1e-4));
    F -= p * (smoothstep(0.12, 0.45, sd) * 5.0 / max(rl, 1e-4));
    // the cursor (an xy cylinder), gravity (screen-down), the hash turbulence
    if (s.pointerKnown){
        vec2 mxy = agent_cursor_magnet(p.xy - swarm_cursor(s), params.mouseRadius * params.mouseRadius, params.mouseInfluence * 3.0);
        F += vec3(mxy, 0.0);
    }
    F += vec3(0.0, -(params.gravity * 1.2), 0.0);
    vec3 rnd = jfx_hash33(p * 193.0 + vec3(s.time * 31.7, s.time * 27.3, seed * 997.0)) - vec3(0.5);
    F += rnd * (params.agitation * 5.0);
    // the injected splats push the swarm away from them (or pull with a negative strength)
    vec2 size = max(s.c.size, vec2(1.0));
    for (int k = 0; k < 8; k++){
        if (k >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(k);
        vec2 sl = vec2((sp.pos.x / size.x - 0.5) * size.x / size.y, -(sp.pos.y / size.y - 0.5));
        float r = sp.radius / size.y;
        F += vec3(agent_cursor_magnet(p.xy - sl, r * r, sp.strength * 6.0), 0.0);
    }
    return F;
}

vec3 swarm_new_velocity(ivec2 cell, JfxStep s){
    vec4 p4 = pos_fetch(cell);
    vec4 v4 = vel_fetch(cell);
    float dt = swarm_dt(s);
    vec3 F = swarm_force(p4.xyz, v4.xyz, p4.w, s);
    float dragMul = exp(-(2.2 + params.damping * 6.0) * dt);
    return agent_euler(v4.xyz, F, dt, dragMul, MAX_SPEED);
}

// the projection of the volume splat: texel centre on the target (box), the size, the brightness
void swarm_project(vec4 p, vec2 target, out vec2 t, out float size, out float bright){
    float persp = 1.0 / clamp(1.0 - p.z * 0.35, 0.45, 2.0);
    float aspect = target.x / target.y;
    float xs = p.x * persp;
    float ys = p.y * persp * -1.0;
    float u = 0.5 + xs / aspect;
    float v = 0.5 + ys;
    t = vec2(u, v) * target;
    size = clamp(params.size * (0.65 + 0.7 * p.w) * persp, 0.6, MAX_SPLAT_SIZE);
    bright = (0.7 + 0.6 * p.w) * persp * persp;
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_SPAWN){
        // makeBallCloudInit (radius 0.55, z squash 0.8)
        float fi = float(agent_index(cell, TABLE));
        vec3 h3 = jfx_hash33(vec3(fi * 1.6180339 + 0.71, fi * 2.7182818 + 4.17, fi * 3.1415926 + 9.03));
        vec3 dir = normalize(h3 - vec3(0.5) + vec3(1e-4, 2e-4, 3e-4));
        float rad = pow(jfx_hash11(fi * 2.2360679 + 1.7), 0.3333) * 0.55;
        float seed = jfx_hash11(fi * 1.7320508 + 0.37);
        return vec4(dir.x * rad, dir.y * rad, dir.z * rad * 0.8, seed);
    }
    if (s.pass == JFX_PASS_DPLACE){
        // the 8 corner cells of each particle's trilinear density splat, one texel each
        ivec2 agentCell = ivec2(cell.x / 8, cell.y);
        int k = cell.x % 8;
        if (float(agent_index(agentCell, TABLE)) >= swarm_count())
            return vec4(0.0);
        ivec3 base;
        vec3 f;
        swarm_grid(pos_fetch(agentCell).xyz, swarm_grid_offset(s), base, f);
        ivec3 corner = base + ivec3(k & 1, (k >> 1) & 1, k >> 2);
        vec2 texel = vec2(swarm_atlas(corner.x, corner.y, corner.z)) + 0.5;
        return vec4(texel, 0.5, 0.5);
    }
    if (s.pass == JFX_PASS_FORCES){
        if (float(agent_index(cell, TABLE)) >= swarm_count())
            return vel_fetch(cell);
        vec3 v3 = swarm_new_velocity(cell, s);
        return vec4(v3, length(v3));
    }
    if (s.pass == JFX_PASS_MOVE){
        vec4 p4 = pos_fetch(cell);
        if (float(agent_index(cell, TABLE)) >= swarm_count())
            return p4;
        vec3 p = clamp(p4.xyz + vel_fetch(cell).xyz * swarm_dt(s), vec3(-1.9), vec3(1.9));
        return vec4(p, p4.w);
    }
    if (s.pass == JFX_PASS_PLACE){
        if (float(agent_index(cell, TABLE)) >= swarm_count())
            return vec4(0.0);
        vec2 target = vec2(accum_size());
        vec2 t;
        float size, bright;
        swarm_project(pos_fetch(cell), target, t, size, bright);
        float rad = ceil(size * sqrt(EXTQ));
        if (!(t.x > -rad && t.x < target.x + rad && t.y > -rad && t.y < target.y + rad))
            return vec4(0.0);
        return agent_texel_window(t, rad, target);
    }
    // the ramp resolve with the exposure (no trails)
    vec4 acc = accum_fetch(cell);
    if (acc.x > 0.0){
        vec4 col = jfx_mix_colors(params.colorA, params.colorB, clamp(acc.y / max(acc.x, 1.0), 0.0, 1.0), params.colorSpace);
        return vec4(col.xyz, agent_energy_alpha(acc.x * (1.0 / 64.0) * params.exposure, 1.6) * col.w);
    }
    return vec4(params.colorA.xyz, 0.0);
}

vec4 jfx_scatter(JfxStep s, JfxPlaced a){
    if (s.pass == JFX_PASS_DENSITY){
        // the corner's trilinear weight in 6-bit fixed point
        ivec2 agentCell = ivec2(a.cell.x / 8, a.cell.y);
        int k = a.cell.x % 8;
        ivec3 base;
        vec3 f;
        swarm_grid(pos_fetch(agentCell).xyz, swarm_grid_offset(s), base, f);
        float wx = mix(1.0 - f.x, f.x, float(k & 1));
        float wy = mix(1.0 - f.y, f.y, float((k >> 1) & 1));
        float wz = mix(1.0 - f.z, f.z, float(k >> 2));
        return vec4(floor(wx * wy * wz * 64.0 + 0.5), 0.0, 0.0, 0.0);
    }
    // the volume splat: the softness-blended profile, its speed-weighted copy
    vec4 p4 = pos_fetch(a.cell);
    vec4 v4 = vel_fetch(a.cell);
    vec2 t;
    float size, bright;
    swarm_project(p4, s.size, t, size, bright);
    float speedNorm = clamp(v4.w * (2.0 / MAX_SPEED), 0.0, 1.0);
    vec2 d = gl_FragCoord.xy - t;
    float q = dot(d, d) / max(size * size, 0.01);
    if (q >= EXTQ)
        return vec4(0.0);
    int shape = int(params.particleShape + 0.5);   // dot|square|glow
    int agentShape = shape == 1 ? 3 : (shape == 2 ? 4 : 2);
    float w;
    if (agentShape == 4)
        w = exp(q * -2.5);
    else
        w = mix(agent_texel_weight(agentShape, q, d.x, d.y, size, 0.9), exp(q * -2.5), params.softness);
    float e = floor(w * bright * 64.0);
    if (e <= 0.0)
        return vec4(0.0);
    bool doSpeed = bright * speedNorm * 64.0 >= 1.0;
    return vec4(e, doSpeed ? floor(w * bright * speedNorm * 64.0) : 0.0, 0.0, 0.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
