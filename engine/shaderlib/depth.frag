#version 450

// A fork may declare a u_fs_customParams block: its members are filled by name from the
// mesh's shader uniforms, the same values the color fork reads. Two names are written by
// the engine instead: "time" (seconds since startup) and "resolution" (xy = shadow slot
// or depth target size, zw = 1 / size). Do not mix int and float members: GL uploads the
// block typed after its first member.
//
//   uniform u_fs_customParams {
//       float time;
//       float dissolve;
//   } customParams;

out vec4 frag_color;
in vec2 v_projZW;

#if defined(HAS_TEXTURE)
    uniform texture2D u_depthTexture;
    uniform sampler u_depth_smp;
    in vec2 v_uv1;
#endif

// Stochastic Tiling (USE_STOCHASTIC_TILING): the alpha clip reads the base colour through the
// same lattice, taps and luminance weights as Lit's colour pass (includes/pbr.glsl, UV set 1),
// so what it cuts out of shadows and depth pre-passes is what the colour pass cuts out
#if defined(USE_STOCHASTIC_TILING) && defined(ALPHA_MASK) && defined(HAS_TEXTURE)
    #define DEPTH_STOCHASTIC
#endif

#if defined(ALPHA_MASK)
    uniform u_fs_depthMaterial {
        vec4 alphaParams; // x = baseColorFactor.a, y = alphaCutoff
        #ifdef DEPTH_STOCHASTIC
            vec4 stochastic; // the material's cell size, blend sharpness, contrast, rotation
        #endif
    } depthMaterial;
#endif

#include "includes/depth_util.glsl"
#ifdef DEPTH_STOCHASTIC
    #include "includes/stochastic.glsl"
#endif

#include "includes/foliage_dither.glsl"

void main() {
    #ifdef USE_FOLIAGE_DITHER
        foliageDitherDiscard();
    #endif
    #if defined(ALPHA_MASK)
        float alpha = depthMaterial.alphaParams.x;
    #endif
    #if defined(DEPTH_STOCHASTIC)
        StochasticTiling stochastic = stochasticSetup(v_uv1, stochasticParams(depthMaterial.stochastic.x,
            depthMaterial.stochastic.y, depthMaterial.stochastic.z, depthMaterial.stochastic.w), 0u);
        vec4 texColor = stochasticTextureLuminance(u_depthTexture, u_depth_smp, stochastic);
        alpha *= texColor.a;
    #elif defined(HAS_TEXTURE)
        vec4 texColor = texture(sampler2D(u_depthTexture, u_depth_smp), v_uv1);

        #if defined(ALPHA_MASK)
            alpha *= texColor.a;
        #endif
    #endif
    #if defined(ALPHA_MASK)
        if (alpha < depthMaterial.alphaParams.y) {
            discard;
        }
    #endif

    // Higher precision equivalent of gl_FragCoord.z in some platforms. See Three.js depth_frag.glsl.js
	frag_color = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);
}
