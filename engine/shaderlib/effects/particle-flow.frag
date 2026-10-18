#version 450
// @effect     particle-flow
// @kind       paint
// @category   interactive
// @summary    Dust carried by a fluid the pointer stirs, riding the eddies it leaves behind
// @source     shaders.com ParticleFlow
// @cost       stateful
// @pointer    trail
// @state      vel rgba16f 256x256 nearest
// @state      pd rgba16f 256x256 nearest
// @state      agents rgba32f 128x128 nearest
// @state      place rgba32f 128x128 nearest
// @state      accum rgba16f box max 1024 nearest
// @state      trail rgba16f box max 1024 linear
// @init       spawn agents
// @step       force vel
// @step       curl vel
// @step       vorticity vel
// @step       divergence pd
// @step       jacobi pd x10
// @step       project vel
// @step       advect-vel vel
// @step       advect agents
// @step       place place
// @step       splat accum scatter place
// @step       resolve trail
// @replay     0.5s
// @suggest-for dust-flow fluid-particles
// @param      count integer 6000 1000 16000 -- Number of drifting particles
// @param      force number 1 0 3 -- How hard the pointer's motion pushes the fluid
// @param      swirl number 25 0 60 -- Vorticity confinement: eddies the dust orbits
// @param      momentum number 0.6 0 1 -- How long the fluid keeps flowing after a stroke
// @param      ambient number 0.15 0 1 -- A gentle breeze that keeps the dust drifting (0: only the pointer)
// @param      speed number 1 0.2 3 -- How strongly the fluid carries the particles
// @param      shape enum streak arrow|streak|dot|square|glow -- How a particle is drawn
// @param      size number 1.2 0.3 3 -- Size of a particle
// @param      trails number 0 0 1 -- How long the motion trails persist
// @param      color-a color #8ec5ff -- Colour of slowly drifting particles
// @param      color-b color #ffd98e -- Colour particles flash toward in fast jets
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the rest-to-excited ramp

// Ported from shaders.com ParticleFlow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/ParticleFlow/index.ts, gpu/scaffolds/fluids.ts with ambientWind, std/sim/agentForces.ts
// advect2d, std/sim/agentRender.ts): a dye-less 256 x 256 Stable-Fluids field stirred by the
// pointer's impulse ribbon and a curl-noise breeze, particles easing toward its velocity with a weak
// R2 home spring, drawn as velocity-oriented comets coloured by local speed into the trail canvas.
// Upstream's idle freeze is not ported (3.2, Out). Accumulators the area's size; sizes upstream's (agent_body).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"
#include "effects/lib/fam_sim_agents.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float count;
    float force;
    float swirl;
    float momentum;
    float ambient;
    float speed;
    float shape;
    float size;
    float trails;
    float colorSpace;
} params;

JFX_STATE(vel)
JFX_STATE(pd)
JFX_STATE(agents)
JFX_STATE(place)
JFX_STATE(accum)
JFX_STATE(trail)

#define FLUID_NO_DYE
#include "effects/lib/fam_sim_fluid.glsl"

const int MAX_PARTICLES = 16384;
const int TABLE = 128;
const float IMPULSE_K = 0.16;
const int MAX_STEPS = 16;
const float FORCE_RADIUS_UV = 0.16;
const float VEL_FADE_MAX = 4.0;
const float VEL_FADE_MIN = 0.05;
const float AMBIENT_K = 26.0;
const float AMBIENT_FREQ = 2.4;
const float AMBIENT_DRIFT = 0.09;
const float INERTIA_RATE = 6.0;
const float SPEED_NORM = 0.35;
const float HOME_RATE = 1.8;
const float SPLAT_R = 14.0;
const vec2 R2_ALPHA = vec2(0.7548776662466927, 0.5698402909980532);

vec4 jfx_paint(JfxContext c){
    return trail_sample(c.uv);
}

#ifdef JFX_STEP_FORM
float flow_dt(JfxStep s){
    return min(max(s.dt, 0.001), 0.033);
}

float flow_count(){
    return clamp(floor(params.count + 0.5), 100.0, float(MAX_PARTICLES));
}

float flow_body(JfxStep s){
    return agent_body(clamp(params.size, 0.3, 3.0) * 1.3, s.c.size.y);
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    float dt = flow_dt(s);
    vec2 size = max(s.c.size, vec2(1.0));
    float aspect = max(size.x / size.y, 0.01);
    float n = 256.0;
    if (s.pass == JFX_PASS_SPAWN){
        // makeUniformScatterInit
        float fi = float(agent_index(cell, TABLE));
        vec3 r = jfx_hash33(vec3(fi * 1.6180339 + 0.71, fi * 2.7182818 + 4.17, fi * 3.1415926 + 9.03));
        return vec4(r.x * aspect, r.y, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_FORCE){
        // the cursor's impulse ribbon (the default tracker: teleport 0.25, minDrag 0.0006)
        vec4 v = vel_fetch(cell);
        vec2 delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / size : vec2(0.0);
        float dragDist = length(delta);
        bool moving = s.pointerKnown && dragDist < 0.25 && dragDist > 0.0006;
        if (moving){
            vec2 velocity = delta / dt * n * params.force * IMPULSE_K;
            float radius = FORCE_RADIUS_UV * n;
            int steps = fluid_ribbon_steps(dragDist, max(0.004, (radius / n) * 0.6), MAX_STEPS);
            vec2 from = s.pointerPrev / size;
            for (int k = 0; k < MAX_STEPS; k++){
                if (k >= steps)
                    break;
                float t = (float(k) + 0.5) / float(steps);
                v = fluid_impulse(cell, (from + delta * t) * n, velocity, radius, v);
            }
        }
        vec2 cellsPerPx = n / size;
        for (int i = 0; i < 8; i++){
            if (i >= s.splatCount)
                break;
            JfxSplat sp = jfx_splat(i);
            v = fluid_impulse(cell, sp.pos * cellsPerPx, sp.vel * cellsPerPx * sp.strength, max(sp.radius * cellsPerPx.x, 1.0), v);
        }
        return v;
    }
    float velFade = VEL_FADE_MAX * pow(VEL_FADE_MIN / VEL_FADE_MAX, clamp(params.momentum, 0.0, 1.0));
    if (s.pass == JFX_PASS_CURL)
        return fluid_curl(cell, false);
    if (s.pass == JFX_PASS_VORTICITY)
        return fluid_vorticity(cell, false, params.swirl, dt, clamp(params.ambient, 0.0, 1.0), AMBIENT_K, AMBIENT_FREQ, s.time * AMBIENT_DRIFT);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return fluid_divergence(cell, false, 0.8);
    if (s.pass == JFX_PASS_JACOBI)
        return fluid_jacobi(cell, false);
    if (s.pass == JFX_PASS_PROJECT)
        return fluid_project(cell, false, 0.0);
    if (s.pass == JFX_PASS_ADVECT_VEL)
        return fluid_advect_vel(cell, false, dt, velFade);
    int i = agent_index(cell, TABLE);
    float fi = float(i);
    if (s.pass == JFX_PASS_ADVECT){
        // advect2d: ease toward the field's velocity, a weak R2 home spring, clamped to the domain
        vec4 a = agents_fetch(cell);
        if (fi >= flow_count())
            return a;
        vec2 uv01 = vec2(a.x / aspect, a.y);
        float cu = uv01.x * n - 0.5;
        float cv = uv01.y * n - 0.5;
        float cx0 = floor(cu);
        float cy0 = floor(cv);
        ivec2 c00 = ivec2(clamp(cx0, 0.0, n - 1.0), clamp(cy0, 0.0, n - 1.0));
        ivec2 c11 = ivec2(clamp(cx0 + 1.0, 0.0, n - 1.0), clamp(cy0 + 1.0, 0.0, n - 1.0));
        float fx = cu - cx0;
        float fy = cv - cy0;
        vec2 v00 = vel_fetch(c00).xy, v10 = vel_fetch(ivec2(c11.x, c00.y)).xy, v01 = vel_fetch(ivec2(c00.x, c11.y)).xy, v11 = vel_fetch(c11).xy;
        vec2 gv = mix(mix(v00, v10, fx), mix(v01, v11, fx), fy);
        vec2 fieldV = vec2(gv.x / n * aspect, gv.y / n) * params.speed;
        float blend = 1.0 - exp(dt * INERTIA_RATE * -1.0);
        float homeX = fract(0.5 + fi * R2_ALPHA.x) * aspect;
        float homeY = fract(0.5 + fi * R2_ALPHA.y);
        float velX = mix(a.z, fieldV.x, blend) + (homeX - a.x) * (HOME_RATE * dt);
        float velY = mix(a.w, fieldV.y, blend) + (homeY - a.y) * (HOME_RATE * dt);
        return vec4(clamp(a.x + velX * dt, 0.0, aspect), clamp(a.y + velY * dt, 0.0, 1.0), velX, velY);
    }
    if (s.pass == JFX_PASS_PLACE){
        if (fi >= flow_count())
            return vec4(0.0);
        vec4 a = agents_fetch(cell);
        vec2 asize = vec2(accum_size());
        AgentProfile p = agent_profile(int(params.shape + 0.5), true);
        float bodyR = clamp(flow_body(s), 1e-6, SPLAT_R / asize.y);
        return agent_window(a.xy, bodyR * p.ext + (0.75 / asize.y) * 2.0, asize);
    }
    // the ramp resolve into the trail canvas
    vec4 acc = accum_fetch(cell);
    vec4 col = params.colorA;
    if (acc.x > 0.0)
        col = jfx_mix_colors(params.colorA, params.colorB, clamp(acc.y / max(acc.x, 1.0), 0.0, 1.0), params.colorSpace);
    float curA = agent_energy_alpha(acc.x * (1.0 / 255.0), 1.6) * col.w;
    float trails = clamp(params.trails, 0.0, 1.0) * 0.92;
    // (upstream bakes the trail read out at trails 0: the same result)
    return trails > 0.0 ? agent_trail_max_decay(col, curA, trail_fetch(cell), trails) : vec4(col.xyz, curA);
}

vec4 jfx_scatter(JfxStep s, JfxPlaced a){
    vec4 ag = agents_fetch(a.cell);
    vec2 v = ag.zw;
    float spd = length(v);
    vec2 dir = spd > 1e-5 ? v / max(spd, 1e-5) : vec2(1.0, 0.0);
    // the excitement from the local speed
    float agitI = clamp(spd / SPEED_NORM, 0.0, 1.0);
    int shape = int(params.shape + 0.5);
    AgentProfile p = agent_profile(shape, true);
    float gain = p.soft > 0.5 ? AGENT_GAIN_SOFT : AGENT_GAIN_HARD;
    float invRes = 1.0 / s.size.y;
    float aaW = 0.75 * invRes;
    float bodyR = clamp(flow_body(s), 1e-6, SPLAT_R * invRes);
    float radW = bodyR * p.ext + aaW * 2.0;
    float offX = gl_FragCoord.x * invRes - ag.x;
    float offY = gl_FragCoord.y * invRes - ag.y;
    float dSq = offX * offX + offY * offY;
    if (dSq >= radW * radW)
        return vec4(0.0);
    float w = agent_oriented_weight(shape, true, dSq, offX, offY, dir, bodyR, aaW);
    float e = floor(w * gain);
    if (e <= 0.0)
        return vec4(0.0);
    return vec4(e, agitI > 0.0 ? floor(w * agitI * gain) : 0.0, 0.0, 0.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
