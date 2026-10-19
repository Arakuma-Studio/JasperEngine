#version 450
// @effect     particle-field
// @kind       filter
// @category   stylize
// @summary    Breaks the content into a breathing 3D relief of particles the pointer throws; they spring home
// @source     shaders.com ParticleField
// @cost       stateful
// @pointer    trail
// @state      pos rgba32f 256x256 nearest
// @state      vel rgba32f 256x256 nearest
// @state      col rgba16f 256x256 nearest
// @state      place rgba32f 256x256 nearest
// @state      accum rgba16f box max 1024 nearest
// @state      field rgba16f box max 1024 linear
// @step       color col
// @step       forces vel
// @step       move pos
// @step       place place
// @step       splat accum scatter place
// @step       resolve field
// @suggest-for relief particle-relief exploded-particles
// @param      count integer 15000 1000 40000 -- Number of particles: how finely the content is sampled into the field
// @param      depth-source enum luminance luminance|luminance-inverted|red|green|blue|saturation|alpha -- Which channel of the content drives each particle's depth
// @param      depth number 0.7 0 2 -- Depth of the relief: how far bright and dark areas rise and sink
// @param      particle-shape enum dot dot|square|glow -- What each particle is drawn as
// @param      particle-size number 0.75 0.3 3 -- Size of each particle relative to the grid spacing
// @param      depth-shading number 0.6 0 1 -- How much far particles dim into the distance
// @param      wobble number 0.35 0 1 -- Gentle idle drift that keeps the field breathing on a still picture
// @param      zoom number 1 0.3 2.5 -- Camera distance: dolly the field closer or further away
// @param      rotation-x angle 0deg -180deg 180deg -- Camera orbit around the horizontal axis
// @param      rotation-y angle 0deg -180deg 180deg -- Camera orbit around the vertical axis
// @param      rotation-z angle 0deg -180deg 180deg -- Camera roll around the view axis
// @param      offset-x number 0 -1 1 -- Shift the field horizontally (in widths)
// @param      offset-y number 0 -1 1 -- Shift the field vertically (in heights)
// @param      cursor-mode enum push none|push|pull -- How the pointer disturbs the field: push throws particles away and toward you, pull gathers them
// @param      cursor-strength number 0.8 0 2 -- How hard the pointer throws particles before they spring back
// @param      cursor-radius number 0.2 0.05 1 -- Reach of the pointer's field (share of the height)

// Ported from shaders.com ParticleField (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/ParticleField/index.ts, std/sim/agentForces.ts integrator.relief + force.cursorInView +
// channel, std/sim/agentRender.ts splat.relief + resolve.weightedColor, std/sim/agentFrame.ts
// fitIsoGrid + cameraRowsYDown): each particle has a home on an iso grid refit to the count and
// the aspect, keeps the content's colour there, and springs toward its home (xy) and a depth from
// a channel of the content (z), thrown by the pointer's magnet worked out in view space; the field
// is projected through the camera rows with perspective and splatted as depth-weighted colour
// (near particles win the average, no sorting). Upstream's one integration kernel is three passes
// here (colour, velocity, position); its fixed-point atomic RGBW accumulators are an additive
// rgba16f float target the area's size (upstream 1024 on the long side), and the resolved field is
// stored premultiplied so its bilinear upsample has no dark fringe.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_state.glsl"
#include "effects/lib/fam_sim_agents.glsl"

uniform u_fs_effectParams {
    float count;
    float depthSource;
    float depth;
    float particleShape;
    float particleSize;
    float depthShading;
    float wobble;
    float zoom;
    float rotationX;
    float rotationY;
    float rotationZ;
    float offsetX;
    float offsetY;
    float cursorMode;
    float cursorStrength;
    float cursorRadius;
} params;

JFX_STATE(pos)
JFX_STATE(vel)
JFX_STATE(col)
JFX_STATE(place)
JFX_STATE(accum)
JFX_STATE(field)

const float GRID_DIM_MAX = 256.0;
const float SPLAT_R = 8.0;
const float AA_W = 0.75;
const float MIN_ALPHA = 0.002;
const float MIN_WEIGHT = 1.0 / 2048.0;
const float WOBBLE_FREQ = 1.15;
const float ALPHA_K = 2.6;
const float PERSP_K = 1.1;
const float STIFF = 26.0;
const float DRAG = 4.2;
const float MAX_SPEED = 4.0;
const float CURSOR_FORCE_K = 10.0;
const float CURSOR_ZFORCE_K = 4.0;

vec4 jfx_filter(JfxContext c){
    return jfx_unpremultiply(field_sample(c.uv));
}

#ifdef JFX_STEP_FORM
struct FieldFrame {
    vec3 rowX, rowY, rowZ;
    float dt, aspect, gridW, gridH, outW, outH, spacing;
    vec2 pointer;
    float cursorForce, cursorZForce, cursorRadSq;
    bool snap;
};

// outSize: the accumulators (a pass writing them reads its own target size: the image it
// writes is not bound for reading)
FieldFrame field_frame(JfxStep s, vec2 outSize){
    FieldFrame f;
    vec2 size = max(s.c.size, vec2(1.0));
    f.dt = min(max(s.dt, 0.001), 0.033);
    f.aspect = size.x / size.y;
    f.snap = s.steps < 1.5;
    // fitIsoGrid(count, aspect, 8, 256)
    float count = clamp(floor(params.count + 0.5), 500.0, 40000.0);
    f.gridW = clamp(floor(sqrt(count * f.aspect) + 0.5), 8.0, GRID_DIM_MAX);
    f.gridH = clamp(floor(count / f.gridW + 0.5), 8.0, GRID_DIM_MAX);
    f.outW = outSize.x;
    f.outH = outSize.y;
    f.spacing = min(f.outW / f.gridW, f.outH / f.gridH);
    // cameraRowsYDown(rx, ry, rz)
    float cx = cos(params.rotationX), sx = sin(params.rotationX);
    float cy = cos(params.rotationY), sy = sin(params.rotationY);
    float cz = cos(params.rotationZ), sz = sin(params.rotationZ);
    float m00 = cz * cy, m01 = cz * sy * sx - sz * cx, m02 = cz * sy * cx + sz * sx;
    float m10 = sz * cy, m11 = sz * sy * sx + cz * cx, m12 = sz * sy * cx - cz * sx;
    float m20 = -sy, m21 = cy * sx, m22 = cy * cx;
    f.rowX = vec3(m00, -m01, m02);
    f.rowY = vec3(-m10, m11, -m12);
    f.rowZ = vec3(m20, -m21, m22);
    // the pointer in uv (no pointer: no cursor force)
    f.pointer = s.pointer / size;
    int mode = int(params.cursorMode + 0.5);   // none|push|pull
    float strength = (mode == 0 || !s.pointerKnown) ? 0.0 : params.cursorStrength;
    float sgn = mode == 2 ? -1.0 : 1.0;
    f.cursorForce = sgn * strength * CURSOR_FORCE_K;
    f.cursorZForce = sgn * strength * CURSOR_ZFORCE_K;
    f.cursorRadSq = params.cursorRadius * params.cursorRadius;
    return f;
}

bool field_in_grid(ivec2 cell, FieldFrame f){
    return float(cell.x) < f.gridW && float(cell.y) < f.gridH;
}

vec2 field_home_uv(ivec2 cell, FieldFrame f){
    return (vec2(cell) + 0.5) / vec2(f.gridW, f.gridH);
}

// the channel menu over the straight colour
float field_channel(vec3 rgb, float a){
    int mode = int(params.depthSource + 0.5);
    if (mode == 1) return 1.0 - dot(rgb, vec3(0.299, 0.587, 0.114));
    if (mode == 2) return rgb.x;
    if (mode == 3) return rgb.y;
    if (mode == 4) return rgb.z;
    if (mode == 5) return max(max(rgb.x, rgb.y), rgb.z) - min(min(rgb.x, rgb.y), rgb.z);
    if (mode == 6) return a;
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

float field_target_z(vec4 colour){
    return (field_channel(colour.xyz, colour.w) - 0.5) * params.depth;
}

// force.cursorInView: the magnet worked out where the particle appears, rotated back by R^T
vec3 field_cursor(vec2 homeW, vec2 off, float z, FieldFrame f){
    vec3 rv = agent_camera_rows(vec3(homeW + off, z), f.rowX, f.rowY, f.rowZ);
    float persp = params.zoom / clamp(1.0 - rv.z * PERSP_K, 0.35, 2.6);
    float su = 0.5 + rv.x * persp / f.aspect + params.offsetX;
    float sv = 0.5 + rv.y * persp + params.offsetY;
    vec2 deltaS = vec2((su - f.pointer.x) * f.aspect, sv - f.pointer.y);
    vec2 mxy = agent_cursor_magnet(deltaS, f.cursorRadSq, f.cursorForce);
    float mz = agent_cursor_falloff(deltaS, f.cursorRadSq) * f.cursorZForce;
    return agent_camera_rows_t(vec3(mxy, mz), f.rowX, f.rowY, f.rowZ);
}

// the latch in vel.w: 0 never saw content, 1 seen, 2 saw content first at this step (snapped)
bool field_needs_snap(float latch, FieldFrame f){
    return f.snap || latch < 0.5;
}

// the projected particle: texel centre, body radius, depth weight; false when it is not drawn
bool field_project(ivec2 cell, FieldFrame f, float time, out vec2 t, out float dotR, out float cw, out float extR){
    vec4 p4 = pos_fetch(cell);
    vec4 colour = col_fetch(cell);
    if (!(colour.w > MIN_ALPHA))
        return false;
    vec2 home = field_home_uv(cell, f);
    float ph = p4.w * JFX_TAU;
    float wob = params.wobble;
    float wx = sin(time * WOBBLE_FREQ + ph) * wob * 0.012;
    float wy = cos(time * WOBBLE_FREQ * 1.13 + ph * 1.7) * wob * 0.012;
    float wz = sin(time * WOBBLE_FREQ * 0.87 + ph * 2.3) * wob * 0.06;
    vec3 pw = vec3((home.x - 0.5) * f.aspect + p4.x + wx, home.y - 0.5 + p4.y + wy, p4.z + wz);
    vec3 rv = agent_camera_rows(pw, f.rowX, f.rowY, f.rowZ);
    float persp0 = 1.0 / clamp(1.0 - rv.z * PERSP_K, 0.35, 2.6);
    float persp = persp0 * params.zoom;
    float su = 0.5 + rv.x * persp / f.aspect + params.offsetX;
    float sv = 0.5 + rv.y * persp + params.offsetY;
    t = vec2(su * f.outW, sv * f.outH);
    dotR = clamp(params.particleSize * f.spacing * 0.5 * persp, 0.5, SPLAT_R);
    float dr = max(params.depth, 1e-3);
    float near = smoothstep(-dr, dr, rv.z);
    float shade = mix(1.0, 0.35 + 0.65 * near, params.depthShading);
    cw = colour.w * persp0 * persp0 * shade;
    int shape = int(params.particleShape + 0.5);   // dot|square|glow
    AgentShape sh = agent_shape(shape == 1 ? 3 : (shape == 2 ? 4 : 2));
    float extShape = sh.soft > 0.5 ? max(sh.ext, 2.4) : sh.ext;
    extR = extShape + AA_W / dotR;
    float rad = ceil(dotR * extR);
    bool onTarget = t.x > -rad && t.x < f.outW + rad && t.y > -rad && t.y < f.outH + rad;
    return cw > MIN_WEIGHT && onTarget;
}

vec4 jfx_step(JfxStep s){
    FieldFrame f = field_frame(s, vec2(accum_size()));
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_COLOR){
        // the content at the home uv (bilinear, straight colour)
        if (!field_in_grid(cell, f))
            return vec4(0.0);
        return jfx_src(field_home_uv(cell, f) * s.c.size);
    }
    if (s.pass == JFX_PASS_FORCES){
        if (!field_in_grid(cell, f))
            return vec4(0.0);
        vec4 p4 = pos_fetch(cell);
        vec4 v4 = vel_fetch(cell);
        vec4 colour = col_fetch(cell);
        bool needsSnap = field_needs_snap(v4.w, f);
        bool seen = v4.w > 0.5 || colour.w > MIN_ALPHA;
        if (needsSnap)
            return vec4(0.0, 0.0, 0.0, seen ? 2.0 : 0.0);
        vec2 home = field_home_uv(cell, f);
        vec2 homeW = vec2((home.x - 0.5) * f.aspect, home.y - 0.5);
        vec2 off = p4.xy;
        vec3 F = vec3(off * -STIFF, (field_target_z(colour) - p4.z) * STIFF) + field_cursor(homeW, off, p4.z, f);
        // the injected splats throw particles along their velocity (px/s -> height/s)
        vec2 size = max(s.c.size, vec2(1.0));
        for (int k = 0; k < 8; k++){
            if (k >= s.splatCount)
                break;
            JfxSplat sp = jfx_splat(k);
            vec2 d = (home - sp.pos / size) * vec2(f.aspect, 1.0) + off;
            float r = max(sp.radius / size.y, 1e-4);
            F += vec3(sp.vel / size.y * sp.strength * exp(-dot(d, d) / (r * r)) * 8.0, 0.0);
        }
        vec3 v3 = agent_euler(v4.xyz, F, f.dt, exp(-DRAG * f.dt), MAX_SPEED);
        return vec4(v3, 1.0);
    }
    if (s.pass == JFX_PASS_MOVE){
        vec4 p4 = pos_fetch(cell);
        if (!field_in_grid(cell, f))
            return p4;
        vec4 v4 = vel_fetch(cell);
        float seed = jfx_hash12(vec2(cell) + 0.5);
        if (f.snap || v4.w < 0.5 || v4.w > 1.5)
            return vec4(0.0, 0.0, field_target_z(col_fetch(cell)), seed);
        return vec4(p4.xy + v4.xy * f.dt, p4.z + v4.z * f.dt, seed);
    }
    if (s.pass == JFX_PASS_PLACE){
        vec2 t;
        float dotR, cw, extR;
        if (!field_in_grid(cell, f) || !field_project(cell, f, s.time, t, dotR, cw, extR))
            return vec4(0.0);
        return agent_texel_window(t, ceil(dotR * extR), vec2(f.outW, f.outH));
    }
    // the weighted-colour resolve, stored premultiplied
    vec4 acc = accum_fetch(cell);
    vec3 rgb = acc.xyz / max(acc.w, MIN_WEIGHT);
    float alpha = agent_energy_alpha(acc.w, ALPHA_K);
    return vec4(rgb * alpha, alpha);
}

vec4 jfx_scatter(JfxStep s, JfxPlaced a){
    // the depth-weighted colour coverage: (r, g, b, w) x weight
    FieldFrame f = field_frame(s, s.size);
    vec2 t;
    float dotR, cw, extR;
    if (!field_project(a.cell, f, s.time, t, dotR, cw, extR))
        return vec4(0.0);
    vec2 d = gl_FragCoord.xy - t;
    float q = dot(d, d) / max(dotR * dotR, 0.01);
    if (q >= extR * extR)
        return vec4(0.0);
    int shape = int(params.particleShape + 0.5);
    float wc = agent_texel_weight(shape == 1 ? 3 : (shape == 2 ? 4 : 2), q, d.x, d.y, dotR, AA_W) * cw;
    if (!(wc > 0.0))
        return vec4(0.0);
    vec4 colour = col_fetch(a.cell);
    return vec4(colour.xyz * wc, wc);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
