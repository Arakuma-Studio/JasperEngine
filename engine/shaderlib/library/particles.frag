#version 450

// @shader particles
// @name "Particles"
// @category "Particles"
// @description "The particle system shader: the particle texture (sprite-sheet frame, blended into the next with Frame Blending) times the particle colour, lit as the Renderer asks (unlit, per-vertex lit or six-way smoke lighting), plus its emission; with alpha-as-light glow, noise erosion over the particle's life, soft intersections and a camera near fade. Draws in the Renderer's blend mode; no depth write. Soft particles read the scene depth: on mobile that is an extra depth pass of the opaque meshes in every camera that sees them (none when SSAO or SSR already made one)."
// @state zwrite=off
// @require scenedepth feature=SOFT
// @feature GLOW "Glow (alpha as light)"
// @feature EROSION "Erosion (the particle's alpha dissolves it through a noise texture)"
// @feature SOFT "Soft particles (fade where they meet geometry; reads the scene depth)"
// @feature NEAR_FADE "Camera near fade"
// @property tint color (1, 1, 1, 1) "Tint"
// @property glowColor hdrcolor (2, 1.3, 0.6, 1) "Glow colour (HDR)" feature=GLOW
// @property glowScale range(0, 4) 1 "Glow alpha scale" feature=GLOW
// @property glowPower range(0.1, 8) 3 "Glow falloff power" feature=GLOW
// @property erosionSoftness range(0.01, 0.5) 0.08 "Erosion edge softness" feature=EROSION
// @property erosionTiling float 1 "Erosion noise tiling (per particle)" feature=EROSION
// @property softness float 0.5 "Softness (world units)" feature=SOFT
// @property nearFadeStart float 0.3 "Near fade: invisible at (world units from the camera)" feature=NEAR_FADE
// @property nearFadeEnd float 1.5 "Near fade: whole from (world units from the camera)" feature=NEAR_FADE
// @texture u_erosionTexture gray "Erosion noise (red channel)" feature=EROSION

// The features and parameters are library:particle's (the Points shader) with the same names
// and meaning, so a migrated Points particle system keeps its look: GLOW adds
// alphaGlow(texture alpha) times the particle colour; EROSION makes the particle's alpha (fading
// over its life) the dissolve threshold through the noise texture, sampled across the
// particle; SOFT and NEAR_FADE scale the final alpha (library/soft_fade.glsl). The colour is
// straight alpha and finalizeStraightColor shapes it for the blend the engine compiles the
// variant with (BLEND_*). The engine's variant bits (Texture, Mesh, Trail, Stretch, Frame
// Blend, Lit, Six-Way, Custom Data, Receive Shadows, Fog) come from the effect's modules.

uniform u_fs_customParams {
    vec4 tint;
    #ifdef GLOW
        vec4 glowColor;
        float glowScale;
        float glowPower;
    #endif
    #ifdef EROSION
        float erosionSoftness;
        float erosionTiling;
    #endif
    #ifdef SOFT
        float softness;
    #endif
    #ifdef NEAR_FADE
        float nearFadeStart;
        float nearFadeEnd;
    #endif
} customParams;

#ifdef EROSION
    uniform texture2D u_erosionTexture;
    uniform sampler u_erosion_smp;
#endif

out vec4 g_finalColor;

#include "includes/particles_fs.glsl"
#include "includes/color_fx.glsl"
#if defined(SOFT) || defined(NEAR_FADE)
    #include "library/soft_fade.glsl"
#endif
#include "includes/output.glsl"

void main() {
    vec4 texel = particleTexel();
    vec4 color = particleSurfaceColor() * customParams.tint;

    #ifdef EROSION
        // the threshold sweeps past the whole noise range as the alpha goes 1 -> 0
        float noise = texture(sampler2D(u_erosionTexture, u_erosion_smp), v_uv * customParams.erosionTiling).r;
        float soft = customParams.erosionSoftness;
        float threshold = mix(-soft, 1.0 + soft, 1.0 - v_color.a);
        color.a *= smoothstep(threshold - soft, threshold + soft, noise);
    #else
        color.a *= v_color.a;
    #endif

    #ifdef GLOW
        color.rgb += alphaGlow(texel.a, customParams.glowScale, customParams.glowPower, customParams.glowColor.rgb) * v_color.rgb;
    #endif

    #ifdef SOFT
        color.a *= softIntersection(customParams.softness);
    #endif
    #ifdef NEAR_FADE
        color.a *= softRamp(softViewDistance(), customParams.nearFadeStart, customParams.nearFadeEnd);
    #endif

    g_finalColor = finalizeStraightColor(color);
}
