// HTML UI effects: main() of a filter effect (@kind filter), included last by the effect file
// after its vec4 jfx_filter(JfxContext c) (straight-alpha linear sRGB). The result mixes with
// the unfiltered pixels by the opacity parameter (0: no effect).
#ifndef JFX_MAIN_FILTER_GLSL
#define JFX_MAIN_FILTER_GLSL

#ifndef JFX_FILTER_FORM
    #error "a filter effect (@kind filter) has the filter form only"
#endif

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"

// (the step forms of a stateful effect have main_step.glsl's main)
#ifndef JFX_STEP_FORM
void main(){
    JfxContext c = jfx_context();
    frag_color = mix(jfx_src_premul_here(), jfx_encode(jfx_filter(c)), clamp(frame.misc.x, 0.0, 1.0));
}
#endif // JFX_STEP_FORM

#endif // JFX_MAIN_FILTER_GLSL
