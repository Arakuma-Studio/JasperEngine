#version 450

// @shader particles
// @name "Particles"
// @category "Particles"
// @description "The particle system shader: the particle texture (sprite-sheet frame, blended into the next with Frame Blending) times the particle colour, lit as the Renderer asks (unlit, per-vertex lit or six-way smoke lighting), plus its emission; with alpha-as-light glow, noise erosion over the particle's life, soft intersections, a camera near fade and distortion. Draws in the Renderer's blend mode; no depth write. Soft particles read the scene depth: on mobile that is an extra depth pass of the opaque meshes in every camera that sees them (none when SSAO or SSR already made one). Distortion reads the scene colour, which holds the opaque objects and the sky only (transparent objects behind a distorting particle are hidden where it is whole): on mobile a copy of the screen and a split of the render pass. Where a camera has no scene colour (a reflection probe's faces) a distorting particle draws as it would without the feature."
// @state zwrite=off
// @require scenedepth feature=SOFT
// @require scenecolor feature=DISTORTION
// @feature GLOW "Glow (alpha as light)"
// @feature EROSION "Erosion (the particle's alpha dissolves it through a noise texture)"
// @feature SOFT "Soft particles (fade where they meet geometry; reads the scene depth)"
// @feature NEAR_FADE "Camera near fade"
// @feature DISTORTION "Distortion (bends the scene behind the particle through a distortion map; reads the scene colour)"
// @property tint color (1, 1, 1, 1) "Tint"
// @property glowColor hdrcolor (2, 1.3, 0.6, 1) "Glow colour (HDR)" feature=GLOW
// @property glowScale range(0, 4) 1 "Glow alpha scale" feature=GLOW
// @property glowPower range(0.1, 8) 3 "Glow falloff power" feature=GLOW
// @property erosionSoftness range(0.01, 0.5) 0.08 "Erosion edge softness" feature=EROSION
// @property erosionTiling float 1 "Erosion noise tiling (per particle)" feature=EROSION
// @property softness float 0.5 "Softness (world units)" feature=SOFT
// @property nearFadeStart float 0.3 "Near fade: invisible at (world units from the camera)" feature=NEAR_FADE
// @property nearFadeEnd float 1.5 "Near fade: whole from (world units from the camera)" feature=NEAR_FADE
// @property distortionStrength range(0, 0.2) 0.03 "Distortion (screen UV)" feature=DISTORTION
// @texture u_erosionTexture gray "Erosion noise (red channel)" feature=EROSION
// @texture u_distortionTexture gray "Distortion map (red: x, green: y; 0.5 shifts nothing)" feature=DISTORTION

// The features and parameters are library:particle's (the Points shader) with the same names
// and meaning, so a migrated Points particle system keeps its look: GLOW adds
// alphaGlow(texture alpha) times the particle colour; EROSION makes the particle's alpha (fading
// over its life) the dissolve threshold through the noise texture, sampled across the
// particle; SOFT and NEAR_FADE scale the final alpha (library/soft_fade.glsl). The colour is
// straight alpha and finalizeStraightColor shapes it for the blend the engine compiles the
// variant with (BLEND_*). The engine's variant bits (Texture, Mesh, Trail, Stretch, Frame
// Blend, Lit, Six-Way, Custom Data, Receive Shadows, Fog) come from the effect's modules.
// DISTORTION shows the scene colour (includes/scene.glsl: the opaque scene and the sky, fogged)
// shifted by the distortion map's red and green at the particle's UV (0.5 shifts nothing) by up
// to distortionStrength screen UV, tinted by the particle colour and the tint, the particle's
// alpha its mask; the texture's colour is not used. It suits the Alpha and Premultiplied blends
// (Additive would add the scene to itself). Where no scene colour was copied (colorParams.z
// is 0) the particle draws its ordinary colour instead.

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
    #ifdef DISTORTION
        float distortionStrength;
    #endif
} customParams;

#ifdef EROSION
    uniform texture2D u_erosionTexture;
    uniform sampler u_erosion_smp;
#endif

#ifdef DISTORTION
    uniform texture2D u_distortionTexture;
    uniform sampler u_distortion_smp;
#endif

out vec4 g_finalColor;

#include "includes/particles_fs.glsl"
#include "includes/color_fx.glsl"
#if defined(SOFT) || defined(NEAR_FADE)
    #include "library/soft_fade.glsl"
#endif
#ifdef DISTORTION
    #include "includes/scene.glsl"
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

    bool fogged = false;
    #ifdef DISTORTION
        if (sceneInputs.colorParams.z > 0.0){
            vec2 screen = screenUV();
            vec2 shift = (texture(sampler2D(u_distortionTexture, u_distortion_smp), v_uv).rg * 2.0 - vec2(1.0)) * customParams.distortionStrength;
            // x scaled by the aspect so the wobble is round
            shift.x *= frame.resolution.y * frame.resolution.z;
            // never past the screen's border, where the copy would repeat its edge
            vec2 room = min(screen, vec2(1.0) - screen);
            shift *= clamp(min(room.x, room.y) / max(2.0 * length(shift), 1e-5), 0.0, 1.0);
            // the scene colour has its fog: the tint fades to white with the fog at the particle
            color.rgb = sceneColor(screen + shift) * fadeByFog(v_color.rgb * customParams.tint.rgb, vec3(1.0));
            fogged = true;
        }
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

    g_finalColor = fogged ? finalizeFoggedColor(color) : finalizeStraightColor(color);
}
