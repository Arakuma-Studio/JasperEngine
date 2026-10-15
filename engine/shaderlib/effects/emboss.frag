#version 450
// @effect     emboss
// @kind       filter
// @category   stylize
// @summary    Raised or sunken relief shading of the content, lit from one side
// @source     shaders.com Emboss (adapted: relief from the content alpha)
// @cost       low
// @suggest-for bevel relief
// @param      depth number -0.5 -1 1 -- Relief depth: negative sinks the content in, positive raises it
// @param      light-angle angle 260deg 0deg 360deg -- Direction the light comes from (highlights and shadows)
// @param      light-intensity number 0.6 0 2 -- Strength of the edge highlights and shadows
// @param      shadow-intensity number 0.3 0 1 -- Darkness of the relief's shadow
// @param      bevel length 3px 0.5px 16px -- Width of the lit edge

// Ported from shaders.com Emboss (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (kit/effects/emboss.ts on the 2D path: embossHeight, embossEdgeLighting, the two-tap cast
// shadow, embossComposite). Adapted: the relief's shape is the content's alpha (the height is
// depth x alpha), the source's 0.003-unit edge stencil is the bevel width and its shadow reach
// (|depth| x 0.06 units, 20 edge stencils) is |depth| x 20 bevels. The source's parallax zoom
// of the content inside the shape is left out: here the shape is the content itself, so the zoom
// would shift the colour off its own alpha.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"

uniform u_fs_effectParams {
    float depth;
    float lightAngle;
    float lightIntensity;
    float shadowIntensity;
    float bevel;
} params;

// lz 0.7: 1 / sqrt(1.49) and lz normalised
#define EMBOSS_INV_LIGHT_LEN 0.8192319205190405
#define EMBOSS_LZN 0.5734623443633283

vec4 jfx_filter(JfxContext c){
    const float EPS = 0.003;           // the source's stencil and edge transition, in its units
    float bevel = max(params.bevel, 0.5);
    vec2 L = vec2(cos(params.lightAngle), sin(params.lightAngle));
    vec4 child = jfx_decode(jfx_src_premul_here());

    // embossEdgeLighting: the height gradient over the bevel, a normal, the directional light
    float hR = params.depth * jfx_src(c.px + vec2(bevel, 0.0)).a;
    float hL = params.depth * jfx_src(c.px - vec2(bevel, 0.0)).a;
    float hU = params.depth * jfx_src(c.px + vec2(0.0, bevel)).a;
    float hD = params.depth * jfx_src(c.px - vec2(0.0, bevel)).a;
    float dhx = (hR - hL) / (EPS * 2.0);
    float dhy = (hU - hD) / (EPS * 2.0);
    float nLen = sqrt(dhx * dhx + dhy * dhy + 1.0);
    vec3 n = vec3(-dhx, -dhy, 1.0) / nLen;
    float NdotL = n.x * L.x * EMBOSS_INV_LIGHT_LEN + n.y * L.y * EMBOSS_INV_LIGHT_LEN + n.z * EMBOSS_LZN;
    float edgeLighting = (NdotL - EMBOSS_LZN) * params.lightIntensity;
    float highlight = max(edgeLighting, 0.0);
    float edgeShadow = max(-edgeLighting, 0.0);

    // the cast shadow: the heights half-way and all the way toward the light against this one
    float absDepth = max(abs(params.depth), 0.0001);
    float spread = absDepth * 20.0 * bevel;
    float hC = params.depth * child.a;
    float h1 = params.depth * jfx_src(c.px + L * spread * 0.5).a;
    float h2 = params.depth * jfx_src(c.px + L * spread).a;
    float castShadow = smoothstep(0.0, 0.5, (h1 - hC) / absDepth) * 0.55 + smoothstep(0.0, 0.5, (h2 - hC) / absDepth) * 0.45;

    float totalShadow = clamp(edgeShadow + castShadow, 0.0, 1.0) * params.shadowIntensity;
    vec3 rgb = clamp(child.rgb * (1.0 - totalShadow) + vec3(highlight), 0.0, 1.0);
    return vec4(rgb, child.a);
}

#include "effects/lib/main_filter.glsl"
