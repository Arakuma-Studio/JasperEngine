#version 450
// @effect     pixel-throw
// @kind       filter
// @category   interactive
// @summary    Pixels thrown along the pointer's path like a fluid, then settling back with friction
// @source     shaders.com PixelThrow
// @cost       stateful
// @pointer    trail
// @state      ptr rgba32f 1x1 nearest
// @state      flow rgba16f 128x128 linear
// @step       track ptr
// @step       throw flow
// @suggest-for pixel-fling smear-pixels
// @param      throw-key enum luminance luminance|darkness|red|green|blue -- Which pixel value decides how far a pixel is thrown
// @param      strength number 0.25 0 1 -- How far pixels can be thrown
// @param      key-influence number 0.8 0 1 -- How much the key modulates the distance (0 all pixels alike)
// @param      radius number 0.2 0.02 0.6 -- Size of the pointer's throw brush (share of the area)
// @param      friction number 0.3 0 1 -- How quickly thrown pixels settle back (0 float almost forever)
// @param      momentum number 0.5 0 1 -- How much the thrown flow keeps drifting after the pointer passes
// @param      edges enum stretch stretch|transparent|mirror|wrap -- How pixels thrown beyond the edges are handled

// Ported from shaders.com PixelThrow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/PixelThrow/index.ts, gpu/kit/sampling.ts bSplineTapSetup). Upstream runs the 128 x 128
// displacement grid on the CPU (hostGridProgram) and uploads it; each cell reads only the previous
// field, so its advect + decay, the brush's injection and the tanh saturation are one fragment pass
// here, the pointer's smoothed velocity in the 1x1 state `ptr` (uv/s, z its speed). dt is clamped
// to 0.016 s as upstream. The content is dragged along the B-spline-filtered field.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float throwKey;
    float strength;
    float keyInfluence;
    float radius;
    float friction;
    float momentum;
    float edges;
} params;

JFX_STATE(ptr)
JFX_STATE(flow)

const float GRID = 128.0;

// bSplineTapSetup(128): the cubic B-spline as four bilinear taps
vec2 throw_flow(vec2 uv){
    float tx = uv.x * GRID - 0.5;
    float ty = uv.y * GRID - 0.5;
    float pxf = floor(tx);
    float pyf = floor(ty);
    float fx = tx - pxf;
    float fy = ty - pyf;
    float ofx = 1.0 - fx;
    float wx0 = ofx * ofx * ofx * 0.16666667;
    float wx1 = fx * fx * fx * 0.5 - fx * fx + 0.66666667;
    float wx3 = fx * fx * fx * 0.16666667;
    float wx2 = 1.0 - wx0 - wx1 - wx3;
    float ofy = 1.0 - fy;
    float wy0 = ofy * ofy * ofy * 0.16666667;
    float wy1 = fy * fy * fy * 0.5 - fy * fy + 0.66666667;
    float wy3 = fy * fy * fy * 0.16666667;
    float wy2 = 1.0 - wy0 - wy1 - wy3;
    float g0x = wx0 + wx1;
    float g1x = wx2 + wx3;
    float g0y = wy0 + wy1;
    float g1y = wy2 + wy3;
    float h0x = pxf - 1.0 + wx1 / g0x;
    float h1x = pxf + 1.0 + wx3 / g1x;
    float h0y = pyf - 1.0 + wy1 / g0y;
    float h1y = pyf + 1.0 + wy3 / g1y;
    vec2 uvA = vec2((h0x + 0.5) / GRID, (h0y + 0.5) / GRID);
    vec2 uvB = vec2((h1x + 0.5) / GRID, (h0y + 0.5) / GRID);
    vec2 uvC = vec2((h0x + 0.5) / GRID, (h1y + 0.5) / GRID);
    vec2 uvD = vec2((h1x + 0.5) / GRID, (h1y + 0.5) / GRID);
    return flow_sample(uvA).xy * (g0x * g0y) + flow_sample(uvB).xy * (g1x * g0y) + flow_sample(uvC).xy * (g0x * g1y) +
           flow_sample(uvD).xy * (g1x * g1y);
}

vec4 jfx_filter(JfxContext c){
    vec2 flowVec = throw_flow(c.uv);
    // the key from the lightly pre-displaced sample
    vec4 probe = jfx_src((c.uv - flowVec * 0.5) * c.size);
    vec3 rgb = probe.rgb;
    float key = dot(rgb, vec3(0.299, 0.587, 0.114));
    if (params.throwKey > 3.5) key = rgb.b;
    else if (params.throwKey > 2.5) key = rgb.g;
    else if (params.throwKey > 1.5) key = rgb.r;
    else if (params.throwKey > 0.5) key = 1.0 - dot(rgb, vec3(0.299, 0.587, 0.114));
    float keyScale = mix(1.0, clamp(key, 0.0, 1.0), params.keyInfluence);
    return jfx_src_edge((c.uv - flowVec * keyScale) * c.size, params.edges);
}

#ifdef JFX_STEP_FORM
// sampleFlow: bilinear, 0 outside the grid
vec2 throw_sample_flow(float gx, float gy){
    float x0 = floor(gx);
    float y0 = floor(gy);
    if (x0 < 0.0 || y0 < 0.0 || x0 + 1.0 >= GRID || y0 + 1.0 >= GRID)
        return vec2(0.0);
    float fx = gx - x0;
    float fy = gy - y0;
    ivec2 c0 = ivec2(int(x0), int(y0));
    return flow_fetch(c0).xy * (1.0 - fx) * (1.0 - fy) + flow_fetch(c0 + ivec2(1, 0)).xy * fx * (1.0 - fy) +
           flow_fetch(c0 + ivec2(0, 1)).xy * (1.0 - fx) * fy + flow_fetch(c0 + ivec2(1, 1)).xy * fx * fy;
}

vec4 jfx_step(JfxStep s){
    float dt = min(s.dt, 0.016);
    vec2 size = max(s.c.size, vec2(1.0));
    if (s.pass == JFX_PASS_TRACK){
        vec2 raw = (s.pointerKnown && dt > 0.0) ? (s.pointer - s.pointerPrev) / size / dt : vec2(0.0);
        vec2 vel = ptr_fetch(ivec2(0)).xy * 0.8 + raw * 0.2;
        return vec4(vel, length(vel), 0.0);
    }
    ivec2 cell = s.cell;
    vec4 p = ptr_fetch(ivec2(0));
    float maxThrow = params.strength * 0.5;
    float decay = max(0.0, 1.0 - params.friction * dt * 4.0);
    float advect = params.momentum * dt * 12.0;
    vec2 f0 = flow_fetch(cell).xy;
    vec2 temp = throw_sample_flow(float(cell.x) - f0.x * GRID * advect, float(cell.y) - f0.y * GRID * advect) * decay;
    // the brush (the Gaussian shifted to reach zero at its radius), aspect-corrected
    float aspect = max(0.0001, size.x / max(1.0, size.y));
    vec2 pointerUv = s.pointer / size;
    vec2 cellUv = (vec2(cell) + 0.5) / GRID;
    if (s.pointerKnown && p.z > 0.02){
        float r = max(0.001, params.radius);
        float r2 = r * r;
        float dx = aspect >= 1.0 ? (cellUv.x - pointerUv.x) * aspect : (cellUv.x - pointerUv.x);
        float dy = aspect >= 1.0 ? (cellUv.y - pointerUv.y) : (cellUv.y - pointerUv.y) / aspect;
        float distSq = dx * dx + dy * dy;
        if (distSq <= r2){
            const float EDGE = 0.36787944117;
            float influence = (exp(-distSq / r2) - EDGE) * (1.0 / (1.0 - EDGE));
            temp += p.xy * influence * dt * 4.0;
        }
    }
    // the injected splats throw along their velocity (uv/s)
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 d = (cellUv - sp.pos / size) * vec2(max(aspect, 1.0), max(1.0 / aspect, 1.0));
        float r = max(sp.radius / size.y, 1e-4);
        float distSq = dot(d, d);
        if (distSq <= r * r)
            temp += sp.vel / size * sp.strength * clamp((exp(-distSq / (r * r)) - 0.36787944117) * (1.0 / (1.0 - 0.36787944117)), 0.0, 1.0) * 0.25;
    }
    // the magnitude's soft saturation
    float len = length(temp);
    if (len > 1e-6)
        temp *= maxThrow > 0.0 ? (maxThrow * tanh(len / maxThrow)) / len : 0.0;
    return vec4(temp, 0.0, 0.0);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
