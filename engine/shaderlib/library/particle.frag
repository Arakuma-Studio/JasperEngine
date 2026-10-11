#version 450

// @shader points
// @name "Particle"
// @category "Particles"
// @description "Point-sprite particles: texture (sprite-sheet rect per point) times the particle colour, with alpha-as-light glow and noise erosion over the particle's life. Draws in the Points component's blend mode (alpha by default; additive, premultiplied and multiply need no other setting); no depth write."
// @state zwrite=off
// @feature GLOW "Glow (alpha as light)"
// @feature EROSION "Erosion (the particle's alpha dissolves it through a noise texture)"
// @property tint color (1, 1, 1, 1) "Tint"
// @property glowColor hdrcolor (2, 1.3, 0.6, 1) "Glow colour (HDR)" feature=GLOW
// @property glowScale range(0, 4) 1 "Glow alpha scale" feature=GLOW
// @property glowPower range(0.1, 8) 3 "Glow falloff power" feature=GLOW
// @property erosionSoftness range(0.01, 0.5) 0.08 "Erosion edge softness" feature=EROSION
// @property erosionTiling float 1 "Erosion noise tiling (per particle)" feature=EROSION
// @texture u_erosionTexture gray "Erosion noise (red channel)" feature=EROSION

// Blending is the Points component's blend mode (Alpha by default); depth writes are off
// so overlapping particles do not cut each other. The colour is straight alpha and
// finalizeStraightColor shapes it for the blend the engine compiles the variant with
// (BLEND_*): Premultiplied gets colour x alpha, Multiply white where the particle is
// transparent, Additive and Alpha the colour as it is. With EROSION the particle's alpha
// (fading over its life) is the dissolve threshold instead of a fade: 1 whole, 0 gone.

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
} customParams;

#ifdef EROSION
    uniform texture2D u_erosionTexture;
    uniform sampler u_erosion_smp;
#endif

out vec4 g_finalColor;

#include "includes/points_fs.glsl"
#include "includes/color_fx.glsl"
#include "includes/output.glsl"

void main() {
    vec2 coord = pointCoord();
    vec4 texel = pointTextureColor(coord);
    vec4 particle = pointVertexColor();

    vec4 color = texel * customParams.tint;
    color.rgb *= particle.rgb;

    #ifdef EROSION
        // the threshold sweeps past the whole noise range as the alpha goes 1 -> 0
        float noise = texture(sampler2D(u_erosionTexture, u_erosion_smp), coord * customParams.erosionTiling).r;
        float soft = customParams.erosionSoftness;
        float threshold = mix(-soft, 1.0 + soft, 1.0 - particle.a);
        color.a *= smoothstep(threshold - soft, threshold + soft, noise);
    #else
        color.a *= particle.a;
    #endif

    #ifdef GLOW
        color.rgb += alphaGlow(texel.a, customParams.glowScale, customParams.glowPower, customParams.glowColor.rgb) * particle.rgb;
    #endif

    g_finalColor = finalizeStraightColor(color);
}
