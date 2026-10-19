#version 450
// @effect     chroma-flow
// @kind       paint
// @category   interactive
// @summary    A liquid that flows behind the pointer, coloured by the direction it moves
// @source     shaders.com ChromaFlow
// @cost       stateful
// @pointer    trail
// @state      ptr rgba32f 1x1 nearest
// @state      field rgba16f 128x128 linear
// @step       track ptr
// @step       flow field
// @suggest-for liquid-flow direction-colors
// @param      base-color color #0066ff -- Base liquid colour
// @param      up-color color #00ff00 -- Colour of upward movement
// @param      down-color color #ff0000 -- Colour of downward movement
// @param      left-color color #0000ff -- Colour of leftward movement
// @param      right-color color #ffff00 -- Colour of rightward movement
// @param      intensity number 1 0.5 1.5 -- Strength of the liquid
// @param      radius number 3 0 5 -- Radius of the liquid under the pointer
// @param      momentum number 30 10 60 -- How long colours keep flowing in their direction

// Ported from shaders.com ChromaFlow (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/ChromaFlow/index.ts, gpu/scaffolds/gridKernels.ts flowDirectionColor). Upstream runs
// the 128 x 128 flow + density grid on the CPU (hostGridProgram) and uploads it; each cell's
// update reads only the previous field, so it is one fragment pass here (advect + decay, then the
// pointer's injection), the pointer's smoothed velocity in the 1x1 state `ptr` (uv/s, w the
// injecting flag). dt is clamped to 0.016 s as upstream.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 baseColor;
    vec4 upColor;
    vec4 downColor;
    vec4 leftColor;
    vec4 rightColor;
    float intensity;
    float radius;
    float momentum;
} params;

JFX_STATE(ptr)
JFX_STATE(field)

const float GRID = 128.0;

// flowDirectionColor: the 5-tap cross of the packed field, coloured by the flow's direction
vec4 jfx_paint(JfxContext c){
    float px = 1.0 / GRID;
    vec4 s0 = field_sample(c.uv);
    vec4 s1 = field_sample(c.uv + vec2(px, 0.0));
    vec4 s2 = field_sample(c.uv + vec2(0.0, px));
    vec4 s3 = field_sample(c.uv + vec2(-px, 0.0));
    vec4 s4 = field_sample(c.uv + vec2(0.0, -px));
    float smoothedLiquid = (s0.z + s1.z + s2.z + s3.z + s4.z) * 0.2;
    float liquidIntensity = smoothstep(0.0, 0.1, smoothedLiquid);
    vec2 flow = (s0.xy + s1.xy + s2.xy + s3.xy + s4.xy) * 0.2;
    float flowMagnitude = length(flow);
    float hasFlow = smoothstep(0.01, 0.1, flowMagnitude);
    float normalizedX = flow.x / (flowMagnitude + 0.001);
    float normalizedY = flow.y / (flowMagnitude + 0.001);
    float rightAmount = smoothstep(0.0, 0.7, max(normalizedX, 0.0));
    float leftAmount = smoothstep(0.0, 0.7, max(normalizedX * -1.0, 0.0));
    float upAmount = smoothstep(0.0, 0.7, max(normalizedY, 0.0));
    float downAmount = smoothstep(0.0, 0.7, max(normalizedY * -1.0, 0.0));
    vec4 horizontalColor = params.leftColor * leftAmount + params.rightColor * rightAmount;
    vec4 verticalColor = params.downColor * downAmount + params.upColor * upAmount;
    float horizontalWeight = leftAmount + rightAmount;
    float verticalWeight = upAmount + downAmount;
    float totalWeight = horizontalWeight + verticalWeight + 0.001;
    vec4 directionalColor = horizontalColor * (horizontalWeight / totalWeight) + verticalColor * (verticalWeight / totalWeight);
    // (upstream returns the colour scaled by the liquid, alpha too, as its straight output)
    return mix(params.baseColor, directionalColor, vec4(hasFlow)) * liquidIntensity;
}

#ifdef JFX_STEP_FORM
float flow_dt(JfxStep s){
    return min(s.dt, 0.016);
}

vec4 jfx_step(JfxStep s){
    float dt = flow_dt(s);
    vec2 size = max(s.c.size, vec2(1.0));
    // the raw velocity of the pointer this step (uv/s)
    vec2 velocity = (s.pointerKnown && dt > 0.0) ? (s.pointer - s.pointerPrev) / size / dt : vec2(0.0);
    if (s.pass == JFX_PASS_TRACK){
        vec2 mouseVel = ptr_fetch(ivec2(0)).xy * 0.85 + velocity * 0.15;
        float injecting = abs(velocity.x) + abs(velocity.y) > 0.01 ? 1.0 : 0.0;
        return vec4(mouseVel, 0.0, injecting);
    }
    // advect + decay
    ivec2 cell = s.cell;
    vec4 p = ptr_fetch(ivec2(0));
    vec2 mouseVel = p.xy;
    bool injecting = p.w > 0.5;
    float radius = params.radius * 0.05;
    float flowFadeRate = 1.0 - dt / max(0.1, 1.0);
    float liquidFadeRate = 1.0 - dt;
    float flowSpeed = params.momentum * 50.0 * dt;
    vec4 f0 = field_fetch(cell);
    vec4 outField = vec4(f0.x * flowFadeRate, f0.y * flowFadeRate, f0.z * liquidFadeRate, 0.0);
    if (abs(f0.x) > 0.001 || abs(f0.y) > 0.001){
        float advectX = float(cell.x) - f0.x * flowSpeed;
        float advectY = float(cell.y) - f0.y * flowSpeed;
        float x0 = floor(advectX);
        float y0 = floor(advectY);
        if (x0 >= 0.0 && y0 >= 0.0 && x0 + 1.0 < GRID && y0 + 1.0 < GRID){
            float fx = advectX - x0;
            float fy = advectY - y0;
            ivec2 c0 = ivec2(int(x0), int(y0));
            float sampled = field_fetch(c0).z * (1.0 - fx) * (1.0 - fy) + field_fetch(c0 + ivec2(1, 0)).z * fx * (1.0 - fy) +
                            field_fetch(c0 + ivec2(0, 1)).z * (1.0 - fx) * fy + field_fetch(c0 + ivec2(1, 1)).z * fx * fy;
            outField.z = sampled * liquidFadeRate;
        }
    }
    // the injection (aspect-corrected distance)
    float aspect = s.c.aspect;
    vec2 pointerUv = s.pointer / size;
    if (injecting && s.pointerKnown){
        float speed = length(mouseVel);
        float speedScale = min(speed * speed * 20.0, 1.0);
        float effectiveRadius = radius * speedScale;
        float maxDistSq = (effectiveRadius * 2.0) * (effectiveRadius * 2.0);
        float radSq = effectiveRadius * effectiveRadius;
        float addScale = params.intensity * 100.0 * dt * 0.01;
        float speedMultiplier = min(speed * 10.0, 1.0);
        vec2 cellUv = (vec2(cell) + 0.5) / GRID;
        float dx = aspect >= 1.0 ? (cellUv.x - pointerUv.x) * aspect : (cellUv.x - pointerUv.x);
        float dy = aspect >= 1.0 ? (cellUv.y - pointerUv.y) : (cellUv.y - pointerUv.y) / aspect;
        float distSq = dx * dx + dy * dy;
        if (distSq < maxDistSq){
            float influence = exp(-distSq / max(radSq, 1e-12));
            outField.x = clamp(outField.x + mouseVel.x * influence * addScale, -1.0, 1.0);
            outField.y = clamp(outField.y + mouseVel.y * influence * addScale, -1.0, 1.0);
            outField.z = clamp(outField.z + influence * addScale * speedMultiplier, 0.0, 1.0);
        }
    }
    // the injected splats: flow along their velocity (uv/s), liquid of their strength
    vec2 cellUv = (vec2(cell) + 0.5) / GRID;
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 d = (cellUv - sp.pos / size) * vec2(max(aspect, 1.0), max(1.0 / aspect, 1.0));
        float r = max(sp.radius / size.y, 1e-4);
        float influence = exp(-dot(d, d) / (r * r));
        outField.xy = clamp(outField.xy + sp.vel / size * influence * 0.1, -1.0, 1.0);
        outField.z = clamp(outField.z + influence * sp.strength, 0.0, 1.0);
    }
    return outField;
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
