#version 450
// the defaults Vulkan GLSL has anyway, stated: hashes, positions and distances need fp32 (R30)
precision highp float;
precision highp int;

// Title Art's generator programs (ShaderType::TITLE_ART with the 'Gen' property bit, docs/specs/
// S83-title-effects-generators.md §6.7): one fragment program per TA_PROGRAM value of
// titleart/gen/programs.glsl. Fullscreen programs write their whole target and address it by
// gl_FragCoord in memory rows; QuadBatch's programs shade the quads and meshes titleart_gen.vert
// pulls from its data texture. No program reads a clock (R16): motion is animated parameters.
//
// The raw images are read with texelFetch through non-filtering samplers (titleart/common.glsl):
// @image_sample_type u_taRaw0 unfilterable_float
// @sampler_type u_taRaw0_smp nonfiltering
// @image_sample_type u_taRaw1 unfilterable_float
// @sampler_type u_taRaw1_smp nonfiltering

#include "titleart/gen/programs.glsl"

out vec4 frag_color;

#if TA_GEN_VERTICES != 0
in vec2 v_quad;
in vec4 v_color;
in vec4 v_extra;
in vec2 v_sizePx;
in vec3 v_more;
#endif

#include "titleart/common.glsl"
#include "titleart/gen/common.glsl"
#include "titleart/gen/tfx_noise.glsl"
#include "titleart/gen/basics.glsl"
#include "titleart/gen/generators.glsl"

void main() {
#if TA_PROGRAM == TA_GEN_PLACE
    frag_color = gen_place();
#elif TA_PROGRAM == TA_GEN_GLOW_ADD
    frag_color = gen_glowAdd();
#elif TA_PROGRAM == TA_GEN_GRID
    frag_color = gen_grid();
#else
    frag_color = gen_program();
#endif
}
