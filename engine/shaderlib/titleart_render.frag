#version 450
// the defaults Vulkan GLSL has anyway, stated: distances, seeds and colours need fp32
precision highp float;
precision highp int;

// The Title Art renderer's passes (docs/specs/S80-title-art-core.md §6.3-§6.7; render/TitleGpu.h
// runs them): ShaderType::TITLE_ART's programs from TA_PROGRAM_RENDERER_FIRST (18) on, the
// variant compiler's choice for those numbers (titleart.frag holds the effect and generator
// programs 1-17); one program per TA_PROGRAM (titleart/render/programs.glsl), each a fullscreen
// triangle inside the pass's viewport and scissor that addresses its target by gl_FragCoord
// (memory rows, titleart/render/common.glsl). A program asks for the texture slots it reads
// (TA_USE_TEXn filterable, TA_USE_RAWn texelFetch); the raw slots are declared unfilterable so
// float targets without linear filtering bind there (sokol validates it).
// @image_sample_type u_taRaw0 unfilterable_float
// @sampler_type u_taRaw0_smp nonfiltering
// @image_sample_type u_taRaw1 unfilterable_float
// @sampler_type u_taRaw1_smp nonfiltering
// @image_sample_type u_taRaw2 unfilterable_float
// @sampler_type u_taRaw2_smp nonfiltering
// @image_sample_type u_taRaw3 unfilterable_float
// @sampler_type u_taRaw3_smp nonfiltering
// @image_sample_type u_taRaw4 unfilterable_float
// @sampler_type u_taRaw4_smp nonfiltering

#include "titleart/render/programs.glsl"

#if TA_PROGRAM == TA_PROGRAM_COPY || TA_PROGRAM == TA_PROGRAM_MASK_VALUE || TA_PROGRAM == TA_PROGRAM_MASK_KEEP
    #define TA_USE_TEX0
    #define TA_USE_TEX1
#elif TA_PROGRAM == TA_PROGRAM_BLEND
    #define TA_USE_TEX0
    #define TA_USE_TEX1
    #define TA_USE_TEX2
#elif TA_PROGRAM == TA_PROGRAM_ADJUST_MIX
    #define TA_USE_TEX0
    #define TA_USE_TEX1
    #define TA_USE_TEX2
    #define TA_USE_TEX3
#elif TA_PROGRAM == TA_PROGRAM_BLUR || TA_PROGRAM == TA_PROGRAM_OUTPUT || TA_PROGRAM == TA_PROGRAM_SEED || TA_PROGRAM == TA_PROGRAM_SEED_ALPHA || TA_PROGRAM == TA_PROGRAM_PLACE || TA_PROGRAM == TA_PROGRAM_GLYPH_COMPOSE
    #define TA_USE_TEX0
#elif TA_PROGRAM == TA_PROGRAM_FLOOD || TA_PROGRAM == TA_PROGRAM_COVER_INIT || TA_PROGRAM == TA_PROGRAM_BLUR_RAW || TA_PROGRAM == TA_PROGRAM_REDUCE_MAX || TA_PROGRAM == TA_PROGRAM_GROW || TA_PROGRAM == TA_PROGRAM_SPAN
    #define TA_USE_RAW0
#elif TA_PROGRAM == TA_PROGRAM_RESOLVE || TA_PROGRAM == TA_PROGRAM_RESOLVE_ALPHA || TA_PROGRAM == TA_PROGRAM_ERODE
    #define TA_USE_TEX0
    #define TA_USE_RAW0
#elif TA_PROGRAM == TA_PROGRAM_HEIGHT
    #define TA_USE_TEX0
    #define TA_USE_RAW0
    #define TA_USE_RAW1
#elif TA_PROGRAM == TA_PROGRAM_COVER_FLOOD || TA_PROGRAM == TA_PROGRAM_COVER_RADIUS
    #define TA_USE_RAW0
    #define TA_USE_RAW1
#elif TA_PROGRAM == TA_PROGRAM_ELEMENT
    #define TA_USE_TEX0
    #define TA_USE_TEX1
    #define TA_USE_TEX2
    #define TA_USE_TEX3
    #define TA_USE_TEX4
    #define TA_USE_TEX5
    #define TA_USE_TEX6
    #define TA_USE_RAW0
    #define TA_USE_RAW1
    #define TA_USE_RAW2
    #define TA_USE_RAW3
    #define TA_USE_RAW4
#elif TA_PROGRAM == TA_PROGRAM_MASK_GRADIENT
    // analytic: no texture
#else
    // the effects' and generators' programs (1-17) are titleart.frag's
    #error "unknown TA_PROGRAM"
#endif

#include "titleart/render/common.glsl"
#include "titleart/render/blend.glsl"

#if TA_PROGRAM == TA_PROGRAM_ERODE || TA_PROGRAM == TA_PROGRAM_ELEMENT
    #include "titleart/render/noise.glsl"
#endif

#include "titleart/render/passes.glsl"
#include "titleart/render/fields.glsl"

#if TA_PROGRAM == TA_PROGRAM_ELEMENT
    // the custom pattern's tile (titleart/patterns.glsl reads it)
    #define TA_PATTERN_IMAGE TA_TEX2
    // a variant's one pattern kind (render/TitleElementVariant.h; the whole program has them all)
    #ifndef TA_EL_WHOLE
        #ifdef TA_EL_PATTERN
            #define TA_PATTERN_ONLY TA_EL_PATTERN
        #else
            #define TA_PATTERN_ONLY 0
        #endif
    #endif
    #include "titleart/patterns.glsl"
    #include "titleart/render/element.glsl"
void main() {
    taElementMain();
}
#endif

#if TA_PROGRAM == TA_PROGRAM_PLACE
// the shaded stack (TA_TEX0, a y-up frame) placed into a node target (rows down, §6.7 pass
// 11): every target pixel maps back through the layer's inverse transform into the frame, with
// taps x taps samples per pixel when the frame is finer than the target. p[2].xy: this target's
// pixel (0, 0) relative to the layer's anchor, output px (fractions of the position included);
// p[2].z output px per canvas px; p[2].w taps; p[3]: the inverse of the layer's linear map
// (document y-up -> layer, x = dot(p[3].xy, q), y = dot(p[3].zw, q)); p[4]: the frame's lower-left
// corner (layer units) and 1 / its size
vec2 taPlaceUv(vec2 frag) {
    vec2 rel = frag + ta.p[2].xy;
    vec2 q = vec2(rel.x, -rel.y) / ta.p[2].z;
    vec2 l = vec2(dot(ta.p[3].xy, q), dot(ta.p[3].zw, q));
    return (l - ta.p[4].xy) * ta.p[4].zw;
}
void main() {
    int taps = int(ta.p[2].w + 0.5);
    vec4 acc = vec4(0.0);
    for (int j = 0; j < taps; j++) for (int i = 0; i < taps; i++) {
        vec2 uv = taPlaceUv(gl_FragCoord.xy + (vec2(i, j) + 0.5) / float(taps) - 0.5);
        if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) acc += texture(TA_TEX0, uv);
    }
    frag_color = acc / float(taps * taps);
}
#endif
