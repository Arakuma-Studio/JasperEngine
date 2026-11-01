#version 450

// @shader mesh
// @name "Unlit"
// @category "Basic"
// @description "Base colour times base map, optionally times vertex colour, without lighting; optional alpha clip, whose shadows are cut out too. Soft intersections and a camera near fade for particles on meshes (instanced quads of a particle system): either one makes it alpha blended without depth writes or shadows. Soft reads the scene depth: on mobile that is an extra depth pass of the opaque meshes in every camera that sees it (none when SSAO or SSR already made one)."
// @require uv1 color
// @require scenedepth feature=SOFT
// @state blend=opaque
// @state[SOFT] blend=alpha zwrite=off shadows=off
// @state[NEAR_FADE] blend=alpha zwrite=off shadows=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff" feature=ALPHA_CLIP
// @property softness float 0.5 "Softness (world units)" feature=SOFT
// @property nearFadeStart float 0.3 "Near fade: invisible at (world units from the camera)" feature=NEAR_FADE
// @property nearFadeEnd float 1.5 "Near fade: whole from (world units from the camera)" feature=NEAR_FADE
// @property stochasticCellSize range(0.25, 4) 1 "Cell Size" feature=STOCHASTIC
// @property stochasticBlendSharpness range(0, 1) 0.4 "Blend Sharpness" feature=STOCHASTIC
// @property stochasticContrast range(0, 1) 0.45 "Contrast" feature=STOCHASTIC
// @property stochasticRotation range(0, 1) 0 "Rotation" feature=STOCHASTIC
// @texture u_baseTexture white "Base map" default[PARTICLE_TEXTURE]=particle
// @feature VERTEX_COLOR "Vertex colour"
// @feature ALPHA_CLIP "Alpha clip"
// @feature SOFT "Soft intersections (fade where it meets geometry; reads the scene depth)"
// @feature NEAR_FADE "Camera near fade"
// @feature STOCHASTIC "Stochastic Tiling"
// @feature PARTICLE_TEXTURE "Built-in soft particle texture (when no base map is set)"
// @depth

// SOFT fades the alpha by saturate((scene depth - fragment depth) / softness): a quad
// crossing the ground or a wall shows no hard line (soft particles on an instanced mesh, a
// glow card). NEAR_FADE fades it out close to the camera (invisible at nearFadeStart, whole
// from nearFadeEnd, view-space distances) without reading the scene. Either one draws alpha
// blended (@state[...]: the transparent queue, which a scene-depth reader must be in), with
// no depth write and no shadows (a soft card has no solid shape); a material blend other
// than opaque still applies (additive glows), and the premultiplied blend fades the whole
// colour. library/soft_fade.glsl has the maths.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    float alphaCutoff;
    #ifdef SOFT
        float softness;
    #endif
    #ifdef NEAR_FADE
        float nearFadeStart;
        float nearFadeEnd;
    #endif
    #ifdef STOCHASTIC
        float stochasticCellSize;
        float stochasticBlendSharpness;
        float stochasticContrast;
        float stochasticRotation;
    #endif
} customParams;

#ifdef STOCHASTIC
    #include "library/stochastic_surface.glsl"
#endif

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

out vec4 g_finalColor;

#if defined(SOFT) || defined(NEAR_FADE)
    #include "library/soft_fade.glsl"
#endif
#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    #ifdef STOCHASTIC
        // the depth program (unlit_depth.frag) reads the same taps, so the clip matches
        StochasticTiling stochastic = stochasticSetup(uv, stochasticCustomParams(), 0u);
        vec4 color = customParams.baseColor * sRGBToLinear(stochasticTextureLuminance(u_baseTexture, u_base_smp, stochastic)) * v_color;
    #else
        vec4 color = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    #endif

    #ifdef ALPHA_CLIP
        if (color.a < customParams.alphaCutoff){
            discard;
        }
    #endif

    #if defined(SOFT) || defined(NEAR_FADE)
        float fade = 1.0;
        #ifdef SOFT
            fade *= softIntersection(customParams.softness);
        #endif
        #ifdef NEAR_FADE
            fade *= softRamp(softViewDistance(), customParams.nearFadeStart, customParams.nearFadeEnd);
        #endif
        // the colour as its texture stores it: premultiplied for that blend, so fade it all
        #ifdef BLEND_PREMULTIPLIED
            color *= fade;
        #else
            color.a *= fade;
        #endif
    #endif

    g_finalColor = finalizeColor(color);
}
