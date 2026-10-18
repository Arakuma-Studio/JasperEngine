#version 450
// @effect     boids
// @kind       paint
// @category   interactive
// @summary    A murmuration of flocking agents; the pointer attracts them or scatters them as a predator
// @source     shaders.com Boids
// @cost       stateful
// @pointer    trail
// @state      agents rgba32f 64x64 nearest
// @state      agit r32f 64x64 nearest
// @state      accel rgba32f 64x64 nearest
// @state      place rgba32f 64x64 nearest
// @state      accum rgba16f box max 1024 nearest
// @state      trail rgba16f box max 1024 linear
// @init       spawn agents
// @step       steer accel
// @step       move agents
// @step       excite agit
// @step       place place
// @step       splat accum scatter place
// @step       resolve trail
// @reseed     seed
// @suggest-for flock murmuration flocking
// @param      count integer 2000 100 4000 -- Number of flocking agents
// @param      speed number 2 0.2 5 -- Cruising speed of the flock
// @param      seed number 0 0 100 -- Re-rolls the flock's starting formation
// @param      size number 1.5 0.5 3 -- Size of each agent (1 is 2 px)
// @param      trails number 0 0 1 -- How long the motion trails persist (0 crisp agents)
// @param      separation number 1.7 0 3 -- How strongly agents steer away from crowding
// @param      alignment number 1.5 0 3 -- How strongly agents match their neighbours' heading
// @param      cohesion number 1 0 3 -- How strongly agents steer toward the local centre
// @param      perception number 0.12 0.05 0.35 -- How far an agent senses its neighbours (share of the height)
// @param      cursor-mode enum repel none|attract|repel -- How the flock reacts to the pointer (repel: a predator)
// @param      cursor-strength number 1.5 0 3 -- Strength of the pointer's pull or push
// @param      agent-shape enum arrow arrow|streak|dot|square|glow -- What an agent is drawn as
// @param      color-a color #8ec5ff -- Colour of agents cruising calmly
// @param      color-b color #ff7ad9 -- Colour agents flash toward when agitated
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the rest-to-excited ramp

// Ported from shaders.com Boids (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/Boids/index.ts, std/sim/agentForces.ts flocking/cursorSteer/wallTurn/steering2d,
// gpu/kit/agents.ts makeClusterFormationInit, std/sim/agentRender.ts oriented splat + ramp resolve):
// Reynolds flocking over a brute-force neighbour scan (4096 agents at most) in world space (x in
// [0, aspect], y in [0, 1]), the agitation envelope, velocity-oriented comet shapes splatted into
// the accumulators and resolved through the rest->excited ramp into the persistent trail canvas.
// Upstream updates the flock in place (Gauss-Seidel); every agent reads the last step's table here
// (the steering is a pass of its own, so the integration and the envelope read it once). The
// accumulators are the area's size (CSS px, upstream a 1024 x 1024 target) and an agent's size is
// in CSS px (upstream's texels of its 1024 reference).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"
#include "effects/lib/fam_sim_agents.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float count;
    float speed;
    float seed;
    float size;
    float trails;
    float separation;
    float alignment;
    float cohesion;
    float perception;
    float cursorMode;
    float cursorStrength;
    float agentShape;
    float colorSpace;
} params;

JFX_STATE(agents)
JFX_STATE(agit)
JFX_STATE(accel)
JFX_STATE(place)
JFX_STATE(accum)
JFX_STATE(trail)

const int MAX_AGENTS = 4096;
const int TABLE = 64;
const float BASE_SPEED = 0.13;
const float FORCE_RATIO = 3.5;
const float AGIT_REST = 1.5;
const float AGIT_GAIN = 0.4545;
const float AGIT_COOL = 1.3;
const float SPLAT_R = 16.0;

vec4 jfx_paint(JfxContext c){
    return trail_sample(c.uv);
}

#ifdef JFX_STEP_FORM
struct BoidsFrame {
    float count, aspect, domainX, maxSpeed, maxForce, perceptionSq, sepRadiusSq;
    vec2 cursor;
    float cursorMode, cursorRadius, cursorRadiusSq, cursorForce;
    float bodyR, margin, turnForce, dt;
};

BoidsFrame boids_frame(JfxStep s){
    BoidsFrame f;
    vec2 size = max(s.c.size, vec2(1.0));
    f.dt = min(max(s.dt, 0.001), 0.033);
    f.aspect = size.x / size.y;
    f.domainX = max(f.aspect, 0.01);
    f.count = clamp(floor(params.count + 0.5), 16.0, float(MAX_AGENTS));
    f.maxSpeed = BASE_SPEED * params.speed;
    f.maxForce = f.maxSpeed * FORCE_RATIO;
    f.perceptionSq = params.perception * params.perception;
    f.sepRadiusSq = (params.perception * 0.5) * (params.perception * 0.5);
    // the pointer in world space (no pointer: no cursor force)
    f.cursor = vec2(s.pointer.x / size.x * f.aspect, s.pointer.y / size.y);
    f.cursorMode = s.pointerKnown ? params.cursorMode : 0.0;
    f.cursorRadius = 0.22;
    f.cursorRadiusSq = f.cursorRadius * f.cursorRadius;
    f.cursorForce = params.cursorStrength * f.maxSpeed * 12.0;
    // the body's half width in world units: `size` x 2 CSS px
    f.bodyR = clamp(params.size, 0.5, 3.0) * 2.0 / size.y;
    f.margin = 0.07;
    f.turnForce = f.maxSpeed * 2.2;
    return f;
}

// the steering of one agent: flocking (one neighbour scan), the cursor, the soft edge turn
vec2 boids_steer(int i, BoidsFrame f){
    vec4 self = agents_fetch(agent_cell(i, TABLE));
    vec2 pos = self.xy;
    vec2 vel = self.zw;
    float maxSpeedI = f.maxSpeed * agent_cruise_variation(float(i));
    vec2 sep = vec2(0.0), ali = vec2(0.0), coh = vec2(0.0);
    float nCount = 0.0, sCount = 0.0;
    for (int j = 0; j < MAX_AGENTS; j++){
        if (float(j) >= f.count)
            break;
        vec4 other = agents_fetch(agent_cell(j, TABLE));
        vec2 off = pos - other.xy;
        float dsq = dot(off, off);
        if (dsq > 1e-7 && dsq < f.perceptionSq){
            ali += other.zw;
            coh += other.xy;
            nCount += 1.0;
            if (dsq < f.sepRadiusSq){
                sep += off / dsq;
                sCount += 1.0;
            }
        }
    }
    vec2 accel = vec2(0.0);
    if (nCount > 0.5){
        vec2 aliSteer = agent_reynolds_steer(ali / nCount, vel, maxSpeedI, f.maxForce);
        vec2 cohSteer = agent_reynolds_steer(coh / nCount - pos, vel, maxSpeedI, f.maxForce);
        accel += aliSteer * params.alignment + cohSteer * params.cohesion;
    }
    if (sCount > 0.5)
        accel += agent_reynolds_steer(sep, vel, maxSpeedI, f.maxForce) * params.separation;
    // cursorSteer: 1 attracts, 2 a predator
    if (f.cursorMode > 0.5){
        vec2 cd = f.cursor - pos;
        float cdsq = dot(cd, cd);
        if (cdsq > 1e-7 && cdsq < f.cursorRadiusSq){
            float cdist = sqrt(cdsq);
            float falloff = 1.0 - cdist / max(f.cursorRadius, 1e-5);
            float sgn = f.cursorMode > 1.5 ? -1.0 : 1.0;
            accel += cd / cdist * (sgn * falloff * f.cursorForce);
        }
    }
    // wallTurn
    float ax = (pos.x < f.margin ? f.turnForce : 0.0) + (pos.x > f.domainX - f.margin ? -f.turnForce : 0.0);
    float ay = (pos.y < f.margin ? f.turnForce : 0.0) + (pos.y > 1.0 - f.margin ? -f.turnForce : 0.0);
    return accel + vec2(ax, ay);
}

// the agent's pose and its splat reach (the oriented world splat)
float boids_reach(BoidsFrame f, vec2 size){
    AgentProfile p = agent_profile(int(params.agentShape + 0.5), true);
    float bodyR = clamp(f.bodyR, 1e-6, SPLAT_R / size.y);
    return bodyR * p.ext + (0.75 / size.y) * 2.0;
}

vec4 jfx_step(JfxStep s){
    BoidsFrame f = boids_frame(s);
    int i = agent_index(s.cell, TABLE);
    if (s.pass == JFX_PASS_SPAWN){
        // makeClusterFormationInit: K elongated clusters with a shared heading
        float fi = float(i);
        float sd = params.seed * 13.137;
        float flocks = max(floor(f.count / 250.0), 3.0);
        float cIdx = fi - floor(fi / flocks) * flocks;
        vec3 rc = jfx_hash33(vec3(cIdx * 7.1315 + sd + 0.71, cIdx * 3.7719 + sd + 4.17, cIdx * 9.4103 + sd + 9.03));
        float ang = rc.z * JFX_TAU;
        vec2 hd = vec2(cos(ang), sin(ang));
        vec2 center = vec2((0.12 + rc.x * 0.76) * f.domainX, 0.12 + rc.y * 0.76);
        vec3 ra = jfx_hash33(vec3(fi * 1.6180339 + sd + 0.71, fi * 2.7182818 + sd + 4.17, fi * 3.1415926 + sd + 9.03));
        float t = (ra.x - 0.5) * 0.6;
        float n = (ra.y - 0.5) * 0.16;
        float px = clamp(center.x + hd.x * t - hd.y * n, 0.0, f.domainX);
        float py = clamp(center.y + hd.y * t + hd.x * n, 0.0, 1.0);
        float jAng = ang + (ra.z - 0.5) * 0.5;
        float sp = f.maxSpeed * (0.75 + ra.y * 0.25);
        return vec4(px, py, cos(jAng) * sp, sin(jAng) * sp);
    }
    if (s.pass == JFX_PASS_STEER){
        if (float(i) >= f.count)
            return vec4(0.0);
        vec2 accel = boids_steer(i, f);
        // the injected splats: a kick along their velocity (px/s -> world/s) within their radius
        vec4 self = agents_fetch(s.cell);
        for (int k = 0; k < 8; k++){
            if (k >= s.splatCount)
                break;
            JfxSplat sp = jfx_splat(k);
            vec2 d = self.xy - sp.pos / s.c.size.y;
            float r = max(sp.radius / s.c.size.y, 1e-4);
            accel += sp.vel / s.c.size.y * sp.strength * exp(-dot(d, d) / (r * r)) * 8.0;
        }
        return vec4(accel, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_MOVE){
        vec4 self = agents_fetch(s.cell);
        if (float(i) >= f.count)
            return self;
        vec2 accel = accel_fetch(s.cell).xy;
        float maxSpeedI = f.maxSpeed * agent_cruise_variation(float(i));
        vec2 vel = self.zw + accel * f.dt;
        float spd = length(vel);
        float clamped = clamp(spd, maxSpeedI * 0.35, maxSpeedI);
        vel = vel / max(spd, 1e-5) * clamped;
        vec2 pos = self.xy + vel * f.dt;
        bool hitX = pos.x < 0.0 || pos.x > f.domainX;
        bool hitY = pos.y < 0.0 || pos.y > 1.0;
        float nvx = hitX ? vel.x * -0.6 : vel.x;
        float nvy = hitY ? vel.y * -0.6 : vel.y;
        return vec4(clamp(pos.x, 0.0, f.domainX), clamp(pos.y, 0.0, 1.0), nvx, nvy);
    }
    if (s.pass == JFX_PASS_EXCITE){
        // the agitation envelope: steering effort above the cruise charges, cools exponentially
        float effort = length(accel_fetch(s.cell).xy) / max(f.maxForce, 1e-5);
        float raw = clamp((effort - AGIT_REST) * AGIT_GAIN, 0.0, 1.0);
        float cooled = agit_fetch(s.cell).x * exp(f.dt * -AGIT_COOL);
        return vec4(max(raw, cooled), 0.0, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_PLACE){
        if (float(i) >= f.count)
            return vec4(0.0);
        vec4 a = agents_fetch(s.cell);
        vec2 size = vec2(accum_size());
        return agent_window(a.xy, boids_reach(f, size), size);
    }
    // the trail canvas: the accumulated energy through the ramp, max-with-decay
    vec4 acc = accum_fetch(s.cell);
    float eRaw = acc.x;
    float sRaw = acc.y;
    float energy = eRaw * (1.0 / 255.0);
    vec4 col = params.colorA;
    if (eRaw > 0.0)
        col = jfx_mix_colors(params.colorA, params.colorB, clamp(sRaw / max(eRaw, 1.0), 0.0, 1.0), params.colorSpace);
    float curA = agent_energy_alpha(energy, 1.6) * col.w;
    return agent_trail_max_decay(col, curA, trail_fetch(s.cell), clamp(params.trails, 0.0, 1.0) * 0.92);
}

vec4 jfx_scatter(JfxStep s, JfxPlaced a){
    // one agent's oriented shape: the energy (x) and the agitation-weighted copy (y), in quanta
    BoidsFrame f = boids_frame(s);
    vec4 ag = agents_fetch(a.cell);
    vec2 pos = ag.xy;
    vec2 vel = ag.zw;
    float spd = length(vel);
    vec2 dir = spd > 1e-5 ? vel / max(spd, 1e-5) : vec2(1.0, 0.0);
    float agitI = clamp(agit_fetch(a.cell).x, 0.0, 1.0);
    int shape = int(params.agentShape + 0.5);
    AgentProfile p = agent_profile(shape, true);
    float gain = p.soft > 0.5 ? AGENT_GAIN_SOFT : AGENT_GAIN_HARD;
    vec2 size = s.size;
    float invRes = 1.0 / size.y;
    float aaW = 0.75 * invRes;
    float bodyR = clamp(f.bodyR, 1e-6, SPLAT_R * invRes);
    float radW = bodyR * p.ext + aaW * 2.0;
    float offX = (gl_FragCoord.x) * invRes - pos.x;
    float offY = (gl_FragCoord.y) * invRes - pos.y;
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
