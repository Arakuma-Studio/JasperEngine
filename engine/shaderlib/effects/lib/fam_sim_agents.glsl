// HTML UI effects (phase 6): the agent family, ported from shaders.com's gpu/kit/agents.ts (the
// shape menu and its coverage profiles, Reynolds steering, the cursor magnet, the director
// torques, the trail composite, the camera rows), std/sim/agentRender.ts (the oriented splat's
// pose, window and per-texel weight; the ramp resolve) and gpu/scaffolds/agentSystem.ts (the
// splat windows, the energy alpha, the semi-implicit Euler), MIT, Copyright (c) 2026 Shader
// Effects Inc.; see effects/LICENSE.shaders-com.txt.
//
// Agent tables are rgba32f state images, one texel per agent (index i at (i % W, i / W)). The
// splat into upstream's fixed-point atomic accumulators is a placement pass (each agent's texel
// window, agent_window) plus a scatter pass blending the coverage additively: the accumulators
// hold upstream's integer quanta (floor(w x gain)), so the resolve divides as upstream does.
#ifndef FAM_SIM_AGENTS_GLSL
#define FAM_SIM_AGENTS_GLSL

#include "effects/lib/jfx_hash.glsl"

const float AGENT_GAIN_HARD = 637.0;
const float AGENT_GAIN_SOFT = 255.0;

// an agent's body half width in world units (the area's height is 1) from upstream's size in
// texels of its 1024 reference: as upstream draws it over an area at least 410 px tall (a smaller
// area shows agents at that 410 px size in CSS px, so they stay legible)
float agent_body(float texels, float areaHeight){
    return texels * max(areaHeight, 410.0) / (1024.0 * max(areaHeight, 1.0));
}

// the agent index of a table texel, and back
int agent_index(ivec2 cell, int width){
    return cell.y * width + cell.x;
}

ivec2 agent_cell(int index, int width){
    return ivec2(index - (index / width) * width, index / width);
}

// ── the shape menu (AGENT_SHAPES): 0 arrow, 1 streak, 2 dot, 3 square, 4 glow ──
struct AgentShape {
    int kind;       // 0 capsule, 1 arrow, 2 box
    float hl, hw;
    float soft;
    float ext;
};

AgentShape agent_shape(int shape){
    AgentShape s;
    if (shape == 0){ s.kind = 1; s.hl = 2.3; s.hw = 1.0; s.soft = 0.0; s.ext = 2.65; }
    else if (shape == 1){ s.kind = 0; s.hl = 1.7; s.hw = 0.8; s.soft = 0.0; s.ext = 2.6; }
    else if (shape == 3){ s.kind = 2; s.hl = 0.9; s.hw = 0.9; s.soft = 0.0; s.ext = 1.5; }
    else if (shape == 4){ s.kind = 0; s.hl = 0.0; s.hw = 0.0; s.soft = 1.0; s.ext = 2.1; }
    else { s.kind = 0; s.hl = 0.0; s.hw = 1.0; s.soft = 0.0; s.ext = 1.2; }
    return s;
}

float agent_arrow_sdf(float t, float n, float hl, float hw){
    float px = abs(n);
    float py = hl - t;
    float qx = hw;
    float qy = hl * 2.0;
    float k = clamp((px * qx + py * qy) / max(qx * qx + qy * qy, 1e-9), 0.0, 1.0);
    float ax = px - qx * k;
    float ay = py - qy * k;
    float bx = px - qx * clamp(px / max(qx, 1e-9), 0.0, 1.0);
    float by = py - qy;
    float dSq = min(ax * ax + ay * ay, bx * bx + by * by);
    float sA = py * qx - px * qy;
    float sB = qy - py;
    bool inside = min(sA, sB) > 0.0;
    return inside ? -sqrt(dSq) : sqrt(dSq);
}

float agent_capsule_sdf(float t, float n, float hl, float r){
    float tc = t - clamp(t, -hl, hl);
    return sqrt(tc * tc + n * n) - r;
}

float agent_box_sdf(float t, float n, float hl, float hw){
    float qx = abs(t) - hl;
    float qy = abs(n) - hw;
    float ox = max(qx, 0.0);
    float oy = max(qy, 0.0);
    return sqrt(ox * ox + oy * oy) + min(max(qx, qy), 0.0);
}

float agent_shape_sdf(AgentShape s, float t, float n, float hl, float hw){
    if (s.kind == 1)
        return agent_arrow_sdf(t, n, hl, hw);
    if (s.kind == 2)
        return agent_box_sdf(t, n, hl, hw);
    return agent_capsule_sdf(t, n, hl, hw);
}

// makeAgentTexelWeightFn: the per-texel coverage (q the squared distance over bodyR^2)
float agent_texel_weight(int shape, float q, float t, float n, float bodyR, float aaW){
    AgentShape s = agent_shape(shape);
    bool isDisc = s.kind == 0 && s.hl == 0.0;
    float sd = isDisc ? bodyR * (sqrt(q) - s.hw) : agent_shape_sdf(s, t, n, bodyR * s.hl, bodyR * s.hw);
    if (s.soft > 0.5){
        float gq = max(sd, 0.0) / max(bodyR * 0.75, 1e-6);
        return exp(gq * gq * -1.0);
    }
    return 1.0 - smoothstep(-aaW, aaW, sd);
}

// makeAgentWeightFn: the hard shape mixed toward the Gaussian skirt by a live softness
float agent_weight_soft(int shape, float t, float n, float bodyR, float aaW, float soft){
    AgentShape s = agent_shape(shape);
    float sd = agent_shape_sdf(s, t, n, bodyR * s.hl, bodyR * s.hw);
    float wHard = 1.0 - smoothstep(-aaW, aaW, sd);
    float gq = max(sd, 0.0) / max(bodyR * 0.75, 1e-6);
    float wSoft = exp(gq * gq * -1.0);
    return mix(wHard, wSoft, soft);
}

// orientedShapeProfile with comet: a glow stretches along the heading
struct AgentProfile {
    float hl, ext, soft;
    bool symmetric;
};

AgentProfile agent_profile(int shape, bool comet){
    AgentShape s = agent_shape(shape);
    AgentProfile p;
    bool glowComet = comet && shape == 4;
    p.soft = s.soft;
    p.hl = glowComet ? 1.0 : s.hl;
    p.ext = glowComet ? s.ext + 1.0 : s.ext;
    p.symmetric = s.kind == 0 && s.hl == 0.0 && !glowComet;
    return p;
}

// makeOrientedCometWeightFn
float agent_comet_weight(int shape, float t, float n, float bodyR, float aaW){
    AgentProfile p = agent_profile(shape, true);
    float hl = bodyR * p.hl;
    float tGlow = t - clamp(t, -hl, hl);
    float tEval = p.soft > 0.5 ? tGlow : t;
    float tk = clamp((hl - t) / max(hl * 2.0, 1e-6), 0.0, 1.0);
    float bright = 1.0 - 0.55 * tk * p.soft;
    float qEval = (tEval * tEval + n * n) / (bodyR * bodyR);
    return agent_texel_weight(shape, qEval, tEval, n, bodyR, aaW) * bright;
}

// the oriented splat's per-texel weight (splatOrientedWorld's texelWeight)
float agent_oriented_weight(int shape, bool comet, float dSq, float offX, float offY, vec2 dir, float bodyR, float aaW){
    AgentProfile p = agent_profile(shape, comet);
    bool useComet = comet && !p.symmetric;
    float t = offX * dir.x + offY * dir.y;
    float n = offX * -dir.y + offY * dir.x;
    if (useComet)
        return agent_comet_weight(shape, t, n, bodyR, aaW);
    if (p.symmetric)
        return agent_texel_weight(shape, dSq / (bodyR * bodyR), offX, offY, bodyR, aaW);
    return agent_texel_weight(shape, dSq / (bodyR * bodyR), t, n, bodyR, aaW);
}

// worldSplatWindow: the texels of an agent at world pos (x in [0, aspect], y in [0, 1]) within
// `reach` world units, on a target of size texels whose height is one world unit; returned as a
// placement (centre in texels, half extents), zero extents when it misses the target
vec4 agent_window(vec2 pos, float reach, vec2 size){
    float res = size.y;
    float cxi = floor(pos.x * res);
    float cyi = floor(pos.y * res);
    float rad = ceil(reach * res);
    float x0 = max(cxi - rad, 0.0), x1 = min(cxi + rad, size.x - 1.0);
    float y0 = max(cyi - rad, 0.0), y1 = min(cyi + rad, size.y - 1.0);
    if (x1 < x0 || y1 < y0)
        return vec4(0.0);
    return vec4((x0 + x1 + 1.0) * 0.5, (y0 + y1 + 1.0) * 0.5, (x1 - x0 + 1.0) * 0.5, (y1 - y0 + 1.0) * 0.5);
}

// texelSplatWindow: an isotropic window of `rad` texels around a texel position
vec4 agent_texel_window(vec2 center, float rad, vec2 size){
    float cxi = floor(center.x);
    float cyi = floor(center.y);
    float x0 = max(cxi - rad, 0.0), x1 = min(cxi + rad, size.x - 1.0);
    float y0 = max(cyi - rad, 0.0), y1 = min(cyi + rad, size.y - 1.0);
    if (x1 < x0 || y1 < y0)
        return vec4(0.0);
    return vec4((x0 + x1 + 1.0) * 0.5, (y0 + y1 + 1.0) * 0.5, (x1 - x0 + 1.0) * 0.5, (y1 - y0 + 1.0) * 0.5);
}

// energyCoverageAlpha
float agent_energy_alpha(float energy, float k){
    return 1.0 - exp(energy * k * -1.0);
}

// integrateSemiImplicitEuler
vec3 agent_euler(vec3 vel, vec3 force, float dt, float dragMul, float maxSpeed){
    vec3 v = (vel + force * dt) * dragMul;
    float spd = length(v);
    return v * (min(spd, maxSpeed) / max(spd, 1e-5));
}

vec2 agent_reynolds_steer(vec2 dir, vec2 vel, float maxSpeed, float maxForce){
    float len = length(dir);
    vec2 desired = len > 1e-5 ? dir / max(len, 1e-5) * maxSpeed : vec2(0.0);
    vec2 steer = desired - vel;
    float sl = length(steer);
    return sl > maxForce ? steer / max(sl, 1e-5) * maxForce : steer;
}

float agent_cruise_variation(float fi){
    return 0.82 + jfx_hash11(fi * 0.61803 + 0.13) * 0.4;
}

vec2 agent_dipole_or_radial(vec2 rhat, vec2 axis, float fieldType){
    float mdotr = dot(axis, rhat);
    vec2 dipoleB = rhat * (2.0 * mdotr) - axis;
    return fieldType > 0.5 ? rhat : dipoleB;
}

float agent_nematic_torque(float k, float target, float theta){
    return k * sin(2.0 * (target - theta));
}

float agent_cursor_falloff(vec2 delta, float radSq){
    return exp(dot(delta, delta) / max(radSq, 1e-5) * -1.0);
}

vec2 agent_cursor_magnet(vec2 delta, float radSq, float force){
    float dl = max(length(delta), 1e-4);
    return delta / dl * (agent_cursor_falloff(delta, radSq) * force);
}

// trailMaxDecay: the brighter of this frame's colour and the decayed trail
vec4 agent_trail_max_decay(vec4 col, float curA, vec4 prev, float trails){
    float prevA = prev.w * trails;
    bool win = curA >= prevA;
    return vec4(win ? col.xyz : prev.xyz, max(curA, prevA));
}

vec3 agent_camera_rows(vec3 pw, vec3 rowX, vec3 rowY, vec3 rowZ){
    return vec3(dot(rowX, pw), dot(rowY, pw), dot(rowZ, pw));
}

vec3 agent_camera_rows_t(vec3 v, vec3 rowX, vec3 rowY, vec3 rowZ){
    return vec3(rowX.x * v.x + rowY.x * v.y + rowZ.x * v.z, rowX.y * v.x + rowY.y * v.y + rowZ.y * v.z,
                rowX.z * v.x + rowY.z * v.y + rowZ.z * v.z);
}

#endif // FAM_SIM_AGENTS_GLSL
