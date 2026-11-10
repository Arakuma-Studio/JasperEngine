#version 450
// the defaults Vulkan GLSL has anyway, stated: distances, seeds and pixel positions need fp32
precision highp float;
precision highp int;

// Title Art's programs (ShaderType::TITLE_ART, docs/specs/S80-title-art-core.md §6.3): one
// fragment program per TA_PROGRAM value (the low property bits, render/TitlePasses.h), each a
// fullscreen pass that writes its whole target (or the scissor of a split step) and addresses it
// by gl_FragCoord in memory rows. TA_SEEDS_FIXED and TA_DISTANCE_FIXED select the RGBA8
// encodings of the jump-flood seeds (and the bounds reduction) and of distances (TitleArtCaps).
//
// The raw images are read with texelFetch through non-filtering samplers (titleart/common.glsl):
// @image_sample_type u_taRaw0 unfilterable_float
// @sampler_type u_taRaw0_smp nonfiltering
// @image_sample_type u_taRaw1 unfilterable_float
// @sampler_type u_taRaw1_smp nonfiltering

#define TA_PROGRAM_COPY 1
#define TA_PROGRAM_GAUSS 2
#define TA_PROGRAM_SEED 3
#define TA_PROGRAM_JFA 4
#define TA_PROGRAM_DISTANCE 5
#define TA_PROGRAM_BOUNDS 6
#define TA_PROGRAM_OUTER_GLOW 7
#define TA_PROGRAM_INNER_GLOW 8
#define TA_PROGRAM_DROP_SHADOW 9
#define TA_PROGRAM_INNER_SHADOW 10
#define TA_PROGRAM_OUTLINE 11
#define TA_PROGRAM_COLOR_OVERLAY 12
#define TA_PROGRAM_GRADIENT_OVERLAY 13
#define TA_PROGRAM_PATTERN_OVERLAY 14
#define TA_PROGRAM_ADJUST 15
#define TA_PROGRAM_FILL 16
#define TA_PROGRAM_PATTERN_FILL 17

out vec4 frag_color;

#include "titleart/common.glsl"
#include "titleart/blend.glsl"
#if TA_PROGRAM == TA_PROGRAM_PATTERN_OVERLAY
    #define TA_PATTERN_IMAGE TA_TEX1
#endif
#if TA_PROGRAM == TA_PROGRAM_PATTERN_OVERLAY || TA_PROGRAM == TA_PROGRAM_PATTERN_FILL
    #include "titleart/patterns.glsl"
#endif
#if TA_PROGRAM == TA_PROGRAM_ADJUST
    #define TA_ADJUST_LUT TA_TEX1
    #include "titleart/adjust.glsl"
#endif
#include "titleart/effects.glsl"

void main() {
#if TA_PROGRAM == TA_PROGRAM_COPY
    frag_color = fx_copy();
#elif TA_PROGRAM == TA_PROGRAM_GAUSS
    frag_color = fx_gauss();
#elif TA_PROGRAM == TA_PROGRAM_SEED
    frag_color = fx_seed();
#elif TA_PROGRAM == TA_PROGRAM_JFA
    frag_color = fx_jfa();
#elif TA_PROGRAM == TA_PROGRAM_DISTANCE
    frag_color = fx_distance();
#elif TA_PROGRAM == TA_PROGRAM_BOUNDS
    frag_color = fx_bounds();
#elif TA_PROGRAM == TA_PROGRAM_OUTER_GLOW
    frag_color = fx_outerGlow();
#elif TA_PROGRAM == TA_PROGRAM_INNER_GLOW
    frag_color = fx_innerGlow();
#elif TA_PROGRAM == TA_PROGRAM_DROP_SHADOW
    frag_color = fx_dropShadow();
#elif TA_PROGRAM == TA_PROGRAM_INNER_SHADOW
    frag_color = fx_innerShadow();
#elif TA_PROGRAM == TA_PROGRAM_OUTLINE
    frag_color = fx_outline();
#elif TA_PROGRAM == TA_PROGRAM_COLOR_OVERLAY
    frag_color = fx_colorOverlay();
#elif TA_PROGRAM == TA_PROGRAM_GRADIENT_OVERLAY
    frag_color = fx_gradientOverlay();
#elif TA_PROGRAM == TA_PROGRAM_PATTERN_OVERLAY
    frag_color = fx_patternOverlay();
#elif TA_PROGRAM == TA_PROGRAM_ADJUST
    frag_color = fx_adjust();
#elif TA_PROGRAM == TA_PROGRAM_FILL
    frag_color = fx_fill();
#elif TA_PROGRAM == TA_PROGRAM_PATTERN_FILL
    frag_color = fx_patternFill();
#else
    #error "titleart.frag: unknown TA_PROGRAM"
#endif
}
