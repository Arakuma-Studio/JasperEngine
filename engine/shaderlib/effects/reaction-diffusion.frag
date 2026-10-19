#version 450
// @effect     reaction-diffusion
// @kind       paint
// @category   interactive
// @summary    A living Gray-Scott reaction-diffusion pattern that blooms where the pointer drags
// @source     shaders.com ReactionDiffusion
// @cost       stateful
// @pointer    trail
// @state      rd rgba32f 512x512 nearest
// @state      pattern rgba16f 512x512 linear
// @init       seed rd
// @step       react rd xspeed
// @step       publish pattern
// @reseed     preset
// @suggest-for gray-scott turing-pattern coral
// @param      preset enum coral coral|mitosis|spots|maze|worms|fingerprints|holes|solitons|bubbles|waves|flowers|custom -- Named Gray-Scott regime (custom uses feed and kill)
// @param      feed number 0.0545 0.01 0.1 -- Feed rate of chemical U (preset custom)
// @param      kill number 0.062 0.03 0.07 -- Kill rate of chemical V (preset custom)
// @param      diffusion-ratio number 0.5 0.2 0.9 -- Ratio of the two diffusion rates (lower is more filament-like)
// @param      feature-size number 3 1 6 -- Size of the pattern's features
// @param      speed integer 6 1 16 -- Simulation iterations a step
// @param      brush-size number 0.04 0.01 0.3 -- Radius of the pointer's brush (share of the field)
// @param      brush-strength number 0.6 0 1 -- How much reagent the moving pointer injects
// @param      color-a color transparent -- Background (empty regions)
// @param      color-c color #38bdf8 -- Colour of the cell walls (mid values)
// @param      color-b color #e2f5ff -- Colour of the cell interiors (high values)
// @param      contrast number 0.5 0 1 -- Sharpness of the cell walls
// @param      threshold number 0.28 0.05 0.6 -- Pattern level the colours are centred on
// @param      color-space enum oklch linear|oklch|oklab|hsl|hsv|lch -- Colour space of the ramp
// @param      relief number 0.4 0 1 -- Shades the pattern as a raised surface (0 flat)
// @param      light-angle angle 135deg 0deg 360deg -- Direction the surface light comes from
// @param      child-influence number 0.6 0 1 -- In filter form: how strongly the content's brightness biases the pattern
// @param      child-contrast number 0.5 0 1 -- In filter form: contrast of the content's brightness
// @param      child-threshold number 0.5 0 1 -- In filter form: the content brightness that reads as neutral
// @param      child-invert boolean false -- In filter form: swap which tones grow the pattern

// Ported from shaders.com ReactionDiffusion (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (shaders/ReactionDiffusion/index.ts, gpu/scaffolds/gridKernels.ts buildGrayScottStep,
// buildScatterSeedKernel, buildPairPublishKernel): the 512 x 512 Gray-Scott field (9-point
// Laplacian, Du 1, dt 1, `speed` iterations), the scatter seed (6 %), the brush while the pointer
// moves, the published field read through the cover fit with the relief lighting. Upstream's
// optional child layer is the element's content in filter form (the step passes read it).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_hash.glsl"
#ifdef JFX_FILTER_FORM
#include "effects/lib/jfx_sample.glsl"
#endif
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorC;
    vec4 colorB;
    float preset;
    float feed;
    float kill;
    float diffusionRatio;
    float featureSize;
    float speed;
    float brushSize;
    float brushStrength;
    float contrast;
    float threshold;
    float colorSpace;
    float relief;
    float lightAngle;
    float childInfluence;
    float childContrast;
    float childThreshold;
    float childInvert;
} params;

JFX_STATE(rd)
JFX_STATE(pattern)

const float N = 512.0;
const float SEED_DENSITY = 0.06;
const float RELIEF_EPS = 1.5 / 512.0;
const float RELIEF_STRENGTH = 6.0;
const float RELIEF_AMBIENT = 0.42;
const float RELIEF_LIGHT_XY = 0.85;
const float RELIEF_LIGHT_Z = 0.55;
const float RELIEF_SHININESS = 18.0;
const float RELIEF_SPEC = 0.35;

// the cover + aspect + feature-size fit of the field over the area
vec2 rd_field_uv(vec2 uv, float aspect){
    float view = 1.0 / max(params.featureSize, 0.0001);
    float sx = min(aspect, 1.0);
    float sy = min(1.0 / aspect, 1.0);
    return vec2((uv.x - 0.5) * sx * view + 0.5, (uv.y - 0.5) * sy * view + 0.5);
}

vec4 jfx_paint(JfxContext c){
    vec2 sampleUV = rd_field_uv(c.uv, c.aspect);
    float V = pattern_sample(sampleUV).y;
    float w = max(0.6 - params.contrast * 0.55, 0.02);
    float t = smoothstep(params.threshold - w, params.threshold + w, V);
    vec4 lowSeg = jfx_mix_colors(params.colorA, params.colorC, clamp(t * 2.0, 0.0, 1.0), params.colorSpace);
    vec4 highSeg = jfx_mix_colors(params.colorC, params.colorB, clamp(t * 2.0 - 1.0, 0.0, 1.0), params.colorSpace);
    vec4 color = t > 0.5 ? highSeg : lowSeg;
    // the relief: V as a height field, one directional light (Lambert + a Blinn sheen)
    float vL = pattern_sample(sampleUV + vec2(-RELIEF_EPS, 0.0)).y;
    float vR = pattern_sample(sampleUV + vec2(RELIEF_EPS, 0.0)).y;
    float vT = pattern_sample(sampleUV + vec2(0.0, -RELIEF_EPS)).y;
    float vB = pattern_sample(sampleUV + vec2(0.0, RELIEF_EPS)).y;
    float gx = (vR - vL) * RELIEF_STRENGTH;
    float gy = (vB - vT) * RELIEF_STRENGTH;
    vec3 n = normalize(vec3(-gx, -gy, 1.0));
    vec3 lightDir = normalize(vec3(cos(params.lightAngle) * RELIEF_LIGHT_XY, sin(params.lightAngle) * RELIEF_LIGHT_XY, RELIEF_LIGHT_Z));
    float diff = clamp(dot(n, lightDir), 0.0, 1.0);
    float shade = RELIEF_AMBIENT + (1.0 - RELIEF_AMBIENT) * diff;
    vec3 halfVec = normalize(lightDir + vec3(0.0, 0.0, 1.0));
    float spec = pow(clamp(dot(n, halfVec), 0.0, 1.0), RELIEF_SHININESS) * RELIEF_SPEC;
    vec3 litRgb = color.rgb * shade + vec3(spec);
    return vec4(mix(color.rgb, litRgb, params.relief), color.a);
}

#ifdef JFX_STEP_FORM
// PRESETS: the (feed, kill) of each regime
vec2 rd_feed_kill(){
    int p = int(params.preset + 0.5);
    if (p == 0) return vec2(0.0545, 0.0620);
    if (p == 1) return vec2(0.0367, 0.0649);
    if (p == 2) return vec2(0.0300, 0.0620);
    if (p == 3) return vec2(0.0290, 0.0570);
    if (p == 4) return vec2(0.0780, 0.0610);
    if (p == 5) return vec2(0.0370, 0.0600);
    if (p == 6) return vec2(0.0390, 0.0580);
    if (p == 7) return vec2(0.0740, 0.0640);
    if (p == 8) return vec2(0.0980, 0.0555);
    if (p == 9) return vec2(0.0140, 0.0450);
    if (p == 10) return vec2(0.0620, 0.06093);
    return vec2(params.feed, params.kill);
}

// the content's drive (filter form): luminance shaped by contrast/threshold/invert, signed, x coverage
float rd_drive(ivec2 cell, vec2 driveMap){
#ifdef JFX_FILTER_FORM
    vec2 f = (vec2(cell) + 0.5) / N;
    vec2 cuv = clamp((f - 0.5) * driveMap + 0.5, vec2(0.0), vec2(1.0));
    vec4 texel = jfx_src(cuv * frame.size.xy);
    float cov = clamp(texel.a, 0.0, 1.0);
    float luma = dot(texel.rgb, vec3(0.2126, 0.7152, 0.0722));
    float gain = 1.0 + params.childContrast * 8.0;
    float shaped = clamp((luma - params.childThreshold) * gain + 0.5, 0.0, 1.0);
    float signedDrive = (shaped - 0.5) * 2.0;
    if (params.childInvert > 0.5)
        signedDrive = -signedDrive;
    return signedDrive * cov;
#else
    return 0.0;
#endif
}

vec4 jfx_step(JfxStep s){
    ivec2 cell = s.cell;
    if (s.pass == JFX_PASS_SEED){
        float h = jfx_hash12(vec2(float(cell.x), float(cell.y)));
        return vec4(1.0, h < SEED_DENSITY ? 1.0 : 0.0, 0.0, 0.0);
    }
    if (s.pass == JFX_PASS_PUBLISH){
        vec4 st = rd_fetch(cell);
        return vec4(st.x, st.y, 0.0, 0.0);
    }
    // one reaction iteration
    vec2 self = rd_fetch(cell).xy;
    vec2 L = rd_fetch(cell + ivec2(-1, 0)).xy;
    vec2 R = rd_fetch(cell + ivec2(1, 0)).xy;
    vec2 T = rd_fetch(cell + ivec2(0, -1)).xy;
    vec2 B = rd_fetch(cell + ivec2(0, 1)).xy;
    vec2 TL = rd_fetch(cell + ivec2(-1, -1)).xy;
    vec2 TR = rd_fetch(cell + ivec2(1, -1)).xy;
    vec2 BL = rd_fetch(cell + ivec2(-1, 1)).xy;
    vec2 BR = rd_fetch(cell + ivec2(1, 1)).xy;
    float lapU = (L.x + R.x + T.x + B.x) * 0.2 + (TL.x + TR.x + BL.x + BR.x) * 0.05 - self.x;
    float lapV = (L.y + R.y + T.y + B.y) * 0.2 + (TL.y + TR.y + BL.y + BR.y) * 0.05 - self.y;
    vec2 fk = rd_feed_kill();
    float view = 1.0 / max(params.featureSize, 0.0001);
    float aspect = s.c.aspect;
    float sx = min(aspect, 1.0);
    float sy = min(1.0 / aspect, 1.0);
    float driveMod = rd_drive(cell, vec2(1.0 / (sx * view), 1.0 / (sy * view))) * params.childInfluence;
    float feedCell = clamp(fk.x + driveMod * 0.022, 0.008, 0.11);
    float killCell = clamp(fk.y - driveMod * 0.014, 0.03, 0.075);
#ifndef JFX_FILTER_FORM
    feedCell = fk.x;
    killCell = fk.y;
#endif
    float U = self.x;
    float V = self.y;
    float uvv = U * V * V;
    float newU = U + (lapU - uvv + feedCell * (1.0 - U));
    float newV = V + (params.diffusionRatio * lapV + uvv - (feedCell + killCell) * V);
    // the brush: a soft Gaussian adding V while the pointer moves (velocity-scaled)
    vec2 size = max(s.c.size, vec2(1.0));
    vec2 delta = s.pointerKnown ? (s.pointer - s.pointerPrev) / size : vec2(0.0);
    float cursorSpeed = length(delta);
    if (cursorSpeed >= 0.25)
        cursorSpeed = 0.0;   // the tracker's teleport guard
    bool moving = cursorSpeed > 0.0005 && params.brushStrength > 0.0;
    float velScale = moving ? min(cursorSpeed / 0.02, 1.0) : 0.0;
    float brushRadGrid = params.brushSize * N * view;
    vec2 field = (s.pointer / size - 0.5) * vec2(sx, sy) * view + 0.5;
    vec2 cellC = vec2(cell) + 0.5;
    if (velScale > 0.0){
        vec2 d = cellC - field * N;
        float inf = exp((dot(d, d) / max(brushRadGrid * brushRadGrid, 1.0)) * -1.0) * velScale;
        newV = min(newV + inf * params.brushStrength, 1.0);
    }
    // the injected splats add reagent (V) under them, in field space
    for (int i = 0; i < 8; i++){
        if (i >= s.splatCount)
            break;
        JfxSplat sp = jfx_splat(i);
        vec2 f = (sp.pos / size - 0.5) * vec2(sx, sy) * view + 0.5;
        float r = max(sp.radius / size.y * N * view, 1.0);
        vec2 d = cellC - f * N;
        newV = min(newV + exp(-dot(d, d) / (r * r)) * sp.strength, 1.0);
    }
    return vec4(clamp(newU, 0.0, 1.0), clamp(newV, 0.0, 1.0), 0.0, 0.0);
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
