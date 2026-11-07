// Post-processing colour (shader library phase 4, tier T0): exposure, tone mapping, colour
// grade, vignette and dither applied at the end of every mesh, library, sky, points and lines
// shader, so the frame needs no post-processing pass for them. UI shaders never apply it.
//
// Only variants compiled with USE_POST_COLOR (one global variant bit, set while the scene's
// PostProcessingSettings need it, RenderSystem::getPostProcessingTiers) declare anything here;
// without it postColorEncode is linearTosRGB and the shaders compile as before.
//
// The colour work is one lookup in a strip LUT the engine bakes on the CPU from the settings
// (u_postColorLut: N*N x N, N 16 or 32, blue across the slices): the linear colour goes in
// through the shaper sqrt(x / (x + k)) (0..inf to 0..1, near-linear in the display range; k
// near 1, RenderSystem::getPostColorShaper) and comes out display-encoded, exposure, curve,
// grade and encoding baked in. When the uber pass runs (FXAA, bloom...) the LUT holds the
// exposure and the curve only (they need the unclamped linear colour) and the uber pass
// grades the frame. Vignette and dither are maths on
// gl_FragCoord; the engine zeroes them when the uber pass does them.
//
// Parameters: four vectors in the block the variant uploads anyway, in this order of
// preference: the built-in Lit's camera block (includes/lit_frame.glsl, before its members),
// the frame block (includes/frame.glsl, after its members), else a block of their own,
// u_fs_postColor. Include this file after those (output.glsl and mesh.frag do).
//
//   postColor_vignetteScaleOffset  xy, zw: p = gl_FragCoord.xy * xy - zw, centred, 1 at the edges
//   postColor_vignetteColor        rgb display colour, a intensity (0 = none)
//   postColor_params               x, y: falloff w = saturate(dot(p, p) * x + y); z dither
//                                  amplitude (0 = none); w LUT side N, 0 = no LUT (a pass
//                                  without post-processing: render-to-texture cameras,
//                                  reflection probes, the editor's Scene view), -N the linear
//                                  store: the colour pass before the SSR composite writes the
//                                  shaper's code alone (no LUT, vignette or dither), linear
//                                  light in the tone curve's range, which the composite decodes
//                                  (postColorStoredToLinear), swaps and encodes with N
//   postColor_shaper               x the shaper's k; y how the draw's blend takes the colour, for
//                                  the built-in shaders (postColorEncodeBlended, no BLEND_*
//                                  define): 0 its own, 1 added, 2 premultiplied (a multiply
//                                  blend's draw gets neutral parameters: the plain encoding)

#ifndef POST_COLOR_GLSL
#define POST_COLOR_GLSL

#include "includes/srgb.glsl"

#ifdef USE_POST_COLOR

    #if defined(LIT_FRAME_DECLARED)
        #define POST_COLOR_BLOCK litFrame
    #elif defined(FRAME_FS_DECLARED)
        #define POST_COLOR_BLOCK frame
    #else
        uniform u_fs_postColor {
            vec4 postColor_vignetteScaleOffset;
            vec4 postColor_vignetteColor;
            vec4 postColor_params;
            vec4 postColor_shaper;
        } postColorBlock;
        #define POST_COLOR_BLOCK postColorBlock
    #endif

    uniform texture2D u_postColorLut;
    uniform sampler u_postColorLut_smp;

    // a colour LUT strip of side n: uvw in 0..1, blue picks the slices (two taps, mixed)
    vec3 samplePostColorLut(vec3 uvw, float n){
        float slice = clamp(uvw.b, 0.0, 1.0) * (n - 1.0);
        float base = floor(slice);
        vec2 rg = clamp(uvw.rg, 0.0, 1.0) * (n - 1.0) + 0.5;
        vec2 uv = vec2((rg.x + base * n) / (n * n), rg.y / n);
        vec3 a = texture(sampler2D(u_postColorLut, u_postColorLut_smp), uv).rgb;
        vec3 b = texture(sampler2D(u_postColorLut, u_postColorLut_smp), uv + vec2(1.0 / n, 0.0)).rgb;
        return mix(a, b, slice - base);
    }

    // interleaved gradient noise, 0..1 (Jimenez): cheap, stable per pixel
    float postColorNoise(vec2 pixel){
        return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
    }

    // the colour pass stores linear light for the SSR composite (params.w < 0)
    bool postColorStoresLinear(){
        return POST_COLOR_BLOCK.postColor_params.w < -0.5;
    }

    // the LUT (exposure, curve, grade, encoding) of a linear colour; the shaper's code alone in
    // the linear store
    vec3 postColorLut(vec3 linearColor){
        vec4 params = POST_COLOR_BLOCK.postColor_params;
        vec3 x = max(linearColor, vec3(0.0));
        float k = POST_COLOR_BLOCK.postColor_shaper.x;
        if (params.w > 0.5)
            return samplePostColorLut(sqrt(x / (x + k)), params.w);
        if (params.w < -0.5)
            return sqrt(x / (x + k));
        return linearTosRGB(x);
    }

    // a colour the linear store wrote, back in linear light (the shaper's inverse, k s^2 / (1 - s^2))
    vec3 postColorStoredToLinear(vec3 stored){
        vec3 s2 = min(stored * stored, vec3(0.999));
        return POST_COLOR_BLOCK.postColor_shaper.x * s2 / (1.0 - s2);
    }

    // the vignette's amount at this fragment (0..1, times its intensity)
    float postColorVignette(){
        vec4 scaleOffset = POST_COLOR_BLOCK.postColor_vignetteScaleOffset;
        vec2 p = gl_FragCoord.xy * scaleOffset.xy - scaleOffset.zw;
        vec4 params = POST_COLOR_BLOCK.postColor_params;
        float w = clamp(dot(p, p) * params.x + params.y, 0.0, 1.0);
        return w * w * (3.0 - 2.0 * w) * POST_COLOR_BLOCK.postColor_vignetteColor.a;
    }

    float postColorDither(){
        return (postColorNoise(gl_FragCoord.xy) - 0.5) * POST_COLOR_BLOCK.postColor_params.z;
    }

    // a shader's own colour: linear in, display-encoded out (the linear store: the vignette and
    // the dither are the composite's)
    vec3 postColorEncode(vec3 linearColor){
        vec3 color = postColorLut(linearColor);
        if (postColorStoresLinear())
            return color;
        color = mix(color, POST_COLOR_BLOCK.postColor_vignetteColor.rgb, postColorVignette());
        return color + vec3(postColorDither());
    }

    // a colour a blend adds to what is behind (additive glows): black adds nothing (a grade's
    // lift is the frame's, not the glow's) and the vignette can only darken it
    vec3 postColorEncodeAdditive(vec3 linearColor){
        // (no dither: what is behind has it, and the target would clamp its negative half)
        float vignette = postColorStoresLinear() ? 0.0 : postColorVignette();
        return max(postColorLut(linearColor) - postColorLut(vec3(0.0)), vec3(0.0)) * (1.0 - vignette);
    }

    // the built-in shaders' colour (mesh, points, lines: no BLEND_* define), linear in,
    // display-encoded out, as the draw's blend takes it (postColor_shaper.y, per draw): its own
    // colour (opaque, alpha), an added one (additive particles: their black adds nothing,
    // not the grade's lifted black), the straight colour's of a premultiplied one; a
    // multiply blend's factor gets neutral parameters (the plain encoding). A uniform branch:
    // the built-in variants have no bit left for the blend.
    vec4 postColorEncodeBlended(vec4 color){
        float blend = POST_COLOR_BLOCK.postColor_shaper.y;
        if (blend > 1.5){
            float coverage = max(color.a, 1e-4);
            return vec4(postColorEncode(color.rgb / coverage) * color.a, color.a);
        }
        if (blend > 0.5)
            return vec4(postColorEncodeAdditive(color.rgb), color.a);
        return vec4(postColorEncode(color.rgb), color.a);
    }

#endif

#endif
