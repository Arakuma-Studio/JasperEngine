#version 450
// @effect     magnetic-filings
// @kind       paint
// @category   interactive
// @summary    Iron filings that swing to align with a magnetic field around the pointer
// @source     shaders.com MagneticFilings
// @cost       stateful
// @pointer    trail
// @state      ptr rgba32f 1x1 nearest
// @state      agents rgba32f 128x96 nearest
// @state      agit r32f 128x96 nearest
// @state      place rgba32f 128x96 nearest
// @state      accum rgba16f box max 1024 nearest
// @state      field rgba16f box max 1024 linear
// @init       spawn agents
// @step       axis ptr
// @step       turn agents
// @step       excite agit
// @step       place place
// @step       splat accum scatter place
// @step       resolve field
// @suggest-for iron-filings magnet compass
// @param      count integer 8000 500 12000 -- Number of filings
// @param      field-type enum dipole dipole|radial -- A bar magnet along the pointer's motion, or a single pole
// @param      strength number 1.7 0 3 -- How strongly the magnet grips the filings
// @param      response number 0.5 0 1 -- How briskly filings swing and settle (low: a long wobble)
// @param      reach number 0.6 0.1 1 -- How far the magnet's influence spreads (share of the height)
// @param      rest-orientation enum random random|horizontal|vertical -- Which way filings point where the field cannot reach them
// @param      shape enum dot arrow|streak|dot|square|glow -- How a filing is drawn
// @param      size number 1 0.5 3 -- Size of a filing (1 is 1.7 px)
// @param      color-a color #be1ef9 -- Colour of filings at rest
// @param      color-b color #6326ff -- Colour filings flash toward as they swing
// @param      color-space enum oklab linear|oklch|oklab|hsl|hsv|lch -- Colour space of the rest-to-excited ramp

// Ported from shaders.com MagneticFilings (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/MagneticFilings/index.ts, std/sim/agentForces.ts dipoleOrRadial/alignToField/rest/
// jitteredGridHome/orientation2d, std/sim/agentFrame.ts createMotionAxis/fitJitteredGrid,
// std/sim/agentRender.ts): nematic directors on a jittered grid that align with the cursor's
// dipole (its axis the smoothed motion, in the 1x1 state `ptr`) or radial field, with angular drag,
// the |omega| envelope and a gentle pull and spring home; angle-oriented shapes splatted and
// resolved through the rest->excited ramp. Accumulators the area's size, sizes in CSS px.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"
#include "effects/lib/fam_sim_agents.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float count;
    float fieldType;
    float strength;
    float response;
    float reach;
    float restOrientation;
    float shape;
    float size;
    float colorSpace;
} params;

JFX_STATE(ptr)
JFX_STATE(agents)
JFX_STATE(agit)
JFX_STATE(place)
JFX_STATE(accum)
JFX_STATE(field)

const int MAX_FILINGS = 12288;
const int TABLE = 128;
const float ALIGN_BASE = 45.0;
const float REST_K = 6.0;
const float DAMP_MIN = 3.0;
const float DAMP_SPAN = 9.0;
const float OMEGA_REF = 6.0;
const float AGIT_COOL = 1.0;
const float HOME_K = 2.5;
const float PULL_K = 0.25;
const float MAX_OFFSET = 0.12;
const float OVERSCAN = 0.14;
const float SPLAT_R = 14.0;

vec4 jfx_paint(JfxContext c){
    return field_sample(c.uv);
}

#ifdef JFX_STEP_FORM
struct FilingsFrame {
    float dt, aspect, domainX, count;
    vec2 cursor, axis;
    float cols, cellW, cellH, damping, bodyR;
};

FilingsFrame filings_frame(JfxStep s){
    FilingsFrame f;
    vec2 size = max(s.c.size, vec2(1.0));
    f.dt = min(max(s.dt, 0.001), 0.033);
    f.aspect = size.x / size.y;
    f.domainX = max(f.aspect, 0.01);
    f.count = clamp(floor(params.count + 0.5), 16.0, float(MAX_FILINGS));
    // fitJitteredGrid over the overscanned domain
    float spanX = f.domainX + 2.0 * OVERSCAN;
    float spanY = 1.0 + 2.0 * OVERSCAN;
    f.cols = max(1.0, floor(sqrt(f.count * (spanX / spanY)) + 0.5));
    float rows = max(1.0, ceil(f.count / f.cols));
    f.cellW = spanX / f.cols;
    f.cellH = spanY / rows;
    f.cursor = vec2(s.pointer.x / size.x * f.aspect, s.pointer.y / size.y);
    f.axis = ptr_fetch(ivec2(0)).xy;
    f.damping = DAMP_MIN + clamp(params.response, 0.0, 1.0) * DAMP_SPAN;
    f.bodyR = clamp(params.size, 0.5, 3.0) * 1.7 / size.y;
    return f;
}

vec2 filings_home(float fi, FilingsFrame f){
    float row = floor(fi / max(f.cols, 1.0));
    float col = fi - row * max(f.cols, 1.0);
    vec2 h = jfx_hash22(vec2(fi * 0.7548 + 0.11, fi * 1.3236 + 5.31));
    vec2 j = (h - 0.5) * 0.85;
    return vec2((col + 0.5 + j.x) * f.cellW - OVERSCAN, (row + 0.5 + j.y) * f.cellH - OVERSCAN);
}

float filings_rest_angle(float fi){
    float randAng = jfx_hash11(fi * 2.3987 + 0.517) * 3.141592653589793;
    float a = params.restOrientation > 0.5 ? 0.0 : randAng;
    return params.restOrientation > 1.5 ? 1.5707963267948966 : a;
}

vec4 jfx_step(JfxStep s){
    FilingsFrame f = filings_frame(s);
    int i = agent_index(s.cell, TABLE);
    float fi = float(i);
    if (s.pass == JFX_PASS_SPAWN){
        float theta0 = jfx_hash11(fi * 1.618034 + 0.31) * 3.141592653589793;
        return vec4(0.0, 0.0, theta0, 0.0);
    }
    if (s.pass == JFX_PASS_AXIS){
        // createMotionAxis (smoothing 0.1, the 0.25 teleport guard): xy the axis, zw the last position
        vec4 p = ptr_fetch(ivec2(0));
        vec2 axis = s.steps <= 1.0 ? vec2(1.0, 0.0) : p.xy;
        vec2 last = s.steps <= 1.0 ? f.cursor : p.zw;
        if (!s.pointerKnown)
            return vec4(axis, last);
        vec2 mv = f.cursor - last;
        float mvLen = length(mv);
        if (mvLen > 1e-3 && mvLen < 0.25){
            axis += (mv / mvLen - axis) * 0.1;
            float al = length(axis);
            axis = al > 0.0 ? axis / al : vec2(1.0, 0.0);
        }
        return vec4(axis, f.cursor);
    }
    if (s.pass == JFX_PASS_TURN){
        vec4 st = agents_fetch(s.cell);
        if (fi >= f.count)
            return st;
        vec2 off = st.xy;
        float theta = st.z;
        float omega = st.w;
        vec2 home = filings_home(fi, f);
        vec2 pos = home + off;
        // the cursor's field (none without a pointer)
        vec2 delta = pos - f.cursor;
        float r = max(length(delta), 1e-4);
        vec2 rhat = delta / r;
        vec2 B = agent_dipole_or_radial(rhat, f.axis, params.fieldType);
        float reachSq = params.reach * params.reach;
        float fieldStr = s.pointerKnown ? params.strength * exp(r * r / max(reachSq, 1e-6) * -1.0) : 0.0;
        // the injected splats: a field of their own (radial, of their strength)
        for (int k = 0; k < 8; k++){
            if (k >= s.splatCount)
                break;
            JfxSplat sp = jfx_splat(k);
            vec2 d = pos - sp.pos / s.c.size.y;
            float rr = max(sp.radius / s.c.size.y, 1e-4);
            float str = sp.strength * exp(-dot(d, d) / (rr * rr));
            if (str > fieldStr){
                fieldStr = str;
                rhat = d / max(length(d), 1e-4);
                B = rhat;
            }
        }
        float phi = atan(B.y, B.x);
        float torque = agent_nematic_torque(ALIGN_BASE * fieldStr, phi, theta) + agent_nematic_torque(REST_K, filings_rest_angle(fi), theta);
        omega = omega + torque * f.dt;
        omega = omega * exp(f.damping * f.dt * -1.0);
        theta = theta + omega * f.dt;
        vec2 pull = rhat * (PULL_K * fieldStr * -1.0);
        off = off + pull * f.dt;
        off = off * exp(HOME_K * f.dt * -1.0);
        float offLen = length(off);
        if (offLen > MAX_OFFSET)
            off = off / max(offLen, 1e-5) * MAX_OFFSET;
        return vec4(off, theta, omega);
    }
    if (s.pass == JFX_PASS_EXCITE){
        float raw = clamp(abs(agents_fetch(s.cell).w) / OMEGA_REF, 0.0, 1.0);
        float cooled = agit_fetch(s.cell).x * exp(f.dt * -AGIT_COOL);
        return vec4(max(raw, cooled), 0.0, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_PLACE){
        if (fi >= f.count)
            return vec4(0.0);
        vec4 a = agents_fetch(s.cell);
        vec2 size = vec2(accum_size());
        AgentProfile p = agent_profile(int(params.shape + 0.5), false);
        float bodyR = clamp(f.bodyR, 1e-6, SPLAT_R / size.y);
        float reach = bodyR * p.ext + (0.75 / size.y) * 2.0;
        return agent_window(filings_home(fi, f) + a.xy, reach, size);
    }
    // the ramp resolve without trails
    vec4 acc = accum_fetch(s.cell);
    if (acc.x > 0.0){
        vec4 col = jfx_mix_colors(params.colorA, params.colorB, clamp(acc.y / max(acc.x, 1.0), 0.0, 1.0), params.colorSpace);
        return vec4(col.xyz, agent_energy_alpha(acc.x * (1.0 / 255.0), 1.6) * col.w);
    }
    return vec4(params.colorA.xyz, 0.0);
}

vec4 jfx_scatter(JfxStep s, JfxPlaced a){
    FilingsFrame f = filings_frame(s);
    vec4 ag = agents_fetch(a.cell);
    vec2 pos = filings_home(float(a.index), f) + ag.xy;
    vec2 dir = vec2(cos(ag.z), sin(ag.z));
    float agitI = clamp(agit_fetch(a.cell).x, 0.0, 1.0);
    int shape = int(params.shape + 0.5);
    AgentProfile p = agent_profile(shape, false);
    float gain = p.soft > 0.5 ? AGENT_GAIN_SOFT : AGENT_GAIN_HARD;
    float invRes = 1.0 / s.size.y;
    float aaW = 0.75 * invRes;
    float bodyR = clamp(f.bodyR, 1e-6, SPLAT_R * invRes);
    float radW = bodyR * p.ext + aaW * 2.0;
    float offX = gl_FragCoord.x * invRes - pos.x;
    float offY = gl_FragCoord.y * invRes - pos.y;
    float dSq = offX * offX + offY * offY;
    if (dSq >= radW * radW)
        return vec4(0.0);
    float w = agent_oriented_weight(shape, false, dSq, offX, offY, dir, bodyR, aaW);
    float e = floor(w * gain);
    if (e <= 0.0)
        return vec4(0.0);
    return vec4(e, agitI > 0.0 ? floor(w * agitI * gain) : 0.0, 0.0, 0.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
