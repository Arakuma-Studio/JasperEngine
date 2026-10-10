#version 450

// @shader mesh
// @name "Panner"
// @category "Effects"
// @description "A texture scrolling across the surface, with an optional second layer (multiplied or added) and noise distortion: lasers, waterfalls, conveyor belts, clouds. Opaque by default; set the blend mode (additive, alpha) on the material."
// @require uv1 color
// @state blend=opaque
// @property tint hdrcolor (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property speed vector2 (0, -0.5) "Scroll speed (UV per second)"
// @property secondTiling vector4 (1, 1, 0, 0) "Second layer tiling (xy) and offset (zw)" feature=SECOND_LAYER
// @property secondSpeed vector2 (0.1, -0.9) "Second layer speed (UV per second)" feature=SECOND_LAYER
// @property distortionStrength range(0, 0.5) 0.05 "Distortion strength (UV)" feature=DISTORTION
// @property distortionTiling vector4 (1, 1, 0, 0) "Distortion tiling (xy) and offset (zw)" feature=DISTORTION
// @property distortionSpeed vector2 (0.05, -0.2) "Distortion speed (UV per second)" feature=DISTORTION
// @texture u_baseTexture white "Texture"
// @texture u_secondTexture white "Second layer" feature=SECOND_LAYER
// @texture u_distortionTexture gray "Distortion noise (red, green)" feature=DISTORTION
// @feature SECOND_LAYER "Second layer"
// @feature DISTORTION "Distortion"
// @feature VERTEX_COLOR "Vertex colour"
// @keywords COMBINE "Combine layers" MULTIPLY ADD

// colour = tint * layer1 (* or + layer2) * vertex colour. Each layer scrolls by its speed
// (the offset wraps every UV unit, so a long run keeps its precision: textures repeat).
// DISTORTION offsets both layers' UVs by a scrolling noise's red and green around 0.5.
// COMBINE ADD adds the layers' colours and alphas (clamped): glows over a base.

uniform u_fs_customParams {
    vec4 tint;
    vec4 tiling;
    vec4 secondTiling;
    vec4 distortionTiling;
    vec2 speed;
    vec2 secondSpeed;
    vec2 distortionSpeed;
    float distortionStrength;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#ifdef SECOND_LAYER
    uniform texture2D u_secondTexture;
    uniform sampler u_second_smp;
#endif

#ifdef DISTORTION
    uniform texture2D u_distortionTexture;
    uniform sampler u_distortion_smp;
#endif

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

out vec4 g_finalColor;

#include "includes/output.glsl"

vec2 pannerScroll(vec2 uv, vec4 tiling, vec2 speed){
    return uv * tiling.xy + tiling.zw + fract(frame.time.x * speed);
}

void main() {
    vec2 uv = v_uv1;
    #ifdef DISTORTION
        vec2 noise = texture(sampler2D(u_distortionTexture, u_distortion_smp),
            pannerScroll(v_uv1, customParams.distortionTiling, customParams.distortionSpeed)).rg;
        uv += (noise - 0.5) * (2.0 * customParams.distortionStrength);
    #endif

    vec4 color = sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), pannerScroll(uv, customParams.tiling, customParams.speed)));

    #ifdef SECOND_LAYER
        vec4 second = sRGBToLinear(texture(sampler2D(u_secondTexture, u_second_smp), pannerScroll(uv, customParams.secondTiling, customParams.secondSpeed)));
        #ifdef COMBINE_ADD
            color = vec4(color.rgb + second.rgb, min(color.a + second.a, 1.0));
        #else
            color *= second;
        #endif
    #endif

    g_finalColor = finalizeColor(customParams.tint * color * v_color);
}
