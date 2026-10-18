#version 450
// @effect     pixel-sort
// @kind       filter
// @category   interactive
// @summary    Pixels sort by brightness around the pointer and keep their place, melting back slowly
// @source     shaders.com PixelSort
// @cost       stateful
// @pointer    position
// @state      luma r32f 512x512 nearest
// @state      offset r32f 512x512 nearest
// @state      tick rgba32f 1x1 nearest
// @step       key luma
// @step       swap offset x5
// @step       count tick
// @suggest-for sort-pixels glitch-sort
// @param      radius number 0.4 0.03 0.6 -- Size of the pointer's sorting brush (share of the area)
// @param      falloff number 1 0 1 -- Softness of the brush edge (0 a hard circle)
// @param      strength number 0.1 0 1 -- How fast pixels sort (more compare-swap passes a step)
// @param      decay number 0.1 0 1 -- How quickly sorted pixels melt back (0 keeps them)
// @param      axis enum vertical horizontal|vertical -- Axis the pixels are sorted along
// @param      direction enum descending ascending|descending -- Sort order by brightness (ascending: dark to light)

// Ported from shaders.com PixelSort (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/PixelSort/index.ts, gpu/scaffolds/gridKernels.ts buildOddEvenSortSet, std/sim/grids.ts
// sortPass): an odd-even transposition sort of a persistent 512 x 512 per-cell displacement map
// keyed by the content's luma, gated by an aspect-corrected brush around the pointer and dithered
// per cell, melting back by `decay`. Upstream runs 1 + round(strength x 4) compare-swap passes a
// frame with the parity carried across frames; here the pass repeats 5 times a step and the
// iterations past that count copy (the 1x1 state `tick` carries the parity and the frame count).
// The brush works while the pointer is known (upstream parks it at the centre).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float radius;
    float falloff;
    float strength;
    float decay;
    float axis;
    float direction;
} params;

JFX_STATE(luma)
JFX_STATE(offset)
JFX_STATE(tick)

const float WORK = 512.0;

vec4 jfx_filter(JfxContext c){
    bool vertical = params.axis > 0.5;
    // the cell's sorted source coordinate along the axis (nearest)
    ivec2 cell = ivec2(clamp(floor(c.uv * WORK), vec2(0.0), vec2(WORK - 1.0)));
    float lineC = vertical ? float(cell.y) : float(cell.x);
    float col = clamp(lineC + offset_fetch(cell).x, 0.0, WORK - 1.0);
    float srcCoord = (col + 0.5) / WORK;
    vec2 childUV = vertical ? vec2(c.uv.x, srcCoord) : vec2(srcCoord, c.uv.y);
    return jfx_src(childUV * c.size);
}

#ifdef JFX_STEP_FORM
int sort_passes(){
    return 1 + int(floor(params.strength * 4.0 + 0.5));
}

// aspectBrushWeight
float sort_brush(float cxf, float cyf, vec2 mouse, float aspect){
    float dx = cxf / WORK - mouse.x;
    float dy = cyf / WORK - mouse.y;
    float ddx = aspect >= 1.0 ? dx * aspect : dx;
    float ddy = aspect >= 1.0 ? dy : dy / aspect;
    float dist = sqrt(ddx * ddx + ddy * ddy);
    float inner = params.radius * (1.0 - params.falloff);
    return 1.0 - smoothstep(inner, params.radius, dist);
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_KEY){
        // the content's luma at the cell (unpremultiplied linear, 601 weights)
        vec3 rgb = jfx_src((vec2(cell) + 0.5) / WORK * s.c.size).rgb;
        return vec4(dot(rgb, vec3(0.299, 0.587, 0.114)), 0.0, 0.0, 0.0);
    }
    vec4 t = tick_fetch(ivec2(0));
    int passes = sort_passes();
    if (s.pass == JFX_PASS_COUNT)
        return vec4(mod(t.x + float(passes), 2.0), mod(t.y + 1.0, 1024.0), 0.0, 0.0);
    // a compare-swap pass (iterations past this step's count leave the map as it is)
    float myOff = offset_fetch(cell).x;
    if (s.iteration >= passes)
        return vec4(myOff, 0.0, 0.0, 0.0);
    bool vertical = params.axis > 0.5;
    float direction = params.direction > 0.5 ? -1.0 : 1.0;
    int parity = int(mod(t.x + float(s.iteration), 2.0));
    float dt = min(s.dt, 0.05);
    float decay = (params.decay * dt * 2.0) / float(passes);
    float maxC = WORK - 1.0;
    int n = int(WORK);
    float xf = float(cell.x);
    float yf = float(cell.y);
    int lineC = vertical ? cell.y : cell.x;
    int fixedC = vertical ? cell.x : cell.y;
    float lineCf = float(lineC);
    float myCol = clamp(lineCf + myOff, 0.0, maxC);
    bool isLeft = ((lineC + parity) % 2) == 0;
    int partnerLineI = isLeft ? lineC + 1 : lineC - 1;
    bool valid = partnerLineI >= 0 && partnerLineI < n;
    int pClamped = clamp(partnerLineI, 0, n - 1);
    ivec2 partner = vertical ? ivec2(fixedC, pClamped) : ivec2(pClamped, fixedC);
    float partnerOff = offset_fetch(partner).x;
    float partnerCol = clamp(float(pClamped) + partnerOff, 0.0, maxC);
    // the key at a fractional line coordinate: the lerp of the two cached cells
    float myC0 = clamp(floor(myCol), 0.0, maxC);
    float myC1 = min(myC0 + 1.0, maxC);
    float myKey = mix(luma_fetch(vertical ? ivec2(fixedC, int(myC0)) : ivec2(int(myC0), fixedC)).x,
                      luma_fetch(vertical ? ivec2(fixedC, int(myC1)) : ivec2(int(myC1), fixedC)).x, myCol - myC0);
    float pC0 = clamp(floor(partnerCol), 0.0, maxC);
    float pC1 = min(pC0 + 1.0, maxC);
    float partnerKey = mix(luma_fetch(vertical ? ivec2(fixedC, int(pC0)) : ivec2(int(pC0), fixedC)).x,
                           luma_fetch(vertical ? ivec2(fixedC, int(pC1)) : ivec2(int(pC1), fixedC)).x, partnerCol - pC0);
    // the brush gate, dithered per cell (no pointer: no brush)
    vec2 size = max(s.c.size, vec2(1.0));
    float aspect = size.x / size.y;
    vec2 mouse = s.pointer / size;
    float wMy = s.pointerKnown ? sort_brush(xf, yf, mouse, aspect) : 0.0;
    float wP = s.pointerKnown ? sort_brush(float(partner.x), float(partner.y), mouse, aspect) : 0.0;
    // the injected splats sort under them too
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 m = sp.pos / size;
        float r = sp.radius / size.y;
        vec2 d1 = (vec2(xf, yf) / WORK - m) * vec2(max(aspect, 1.0), max(1.0 / aspect, 1.0));
        vec2 d2 = (vec2(partner) / WORK - m) * vec2(max(aspect, 1.0), max(1.0 / aspect, 1.0));
        wMy = max(wMy, (1.0 - smoothstep(0.0, r, length(d1))) * sp.strength);
        wP = max(wP, (1.0 - smoothstep(0.0, r, length(d2))) * sp.strength);
    }
    float rnd = jfx_hash13(vec3(xf, yf, t.y));
    bool gate = valid && min(wMy, wP) > rnd;
    float keyDiff = (myKey - partnerKey) * direction;
    bool takePartner = isLeft ? keyDiff > 0.0 : keyDiff < 0.0;
    float newCol = (gate && takePartner) ? partnerCol : myCol;
    float decayed = mix(newCol, lineCf, decay);
    return vec4(decayed - lineCf, 0.0, 0.0, 0.0);
}
#endif

#include "effects/lib/main_filter.glsl"
#include "effects/lib/main_step.glsl"
