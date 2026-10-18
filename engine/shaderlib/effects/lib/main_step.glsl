// HTML UI effects (phase 6): main() of a stateful effect's step forms, included last by the effect
// file (after its main_paint.glsl or main_filter.glsl include, whose main() the step forms leave
// out). The step form writes jfx_step(s) into the pass's target texel, the scatter form
// jfx_scatter(s, a) into the texels of the agent's quad (blended additively): raw values, not
// colours. In the draw forms this file adds nothing.
#ifndef JFX_MAIN_STEP_GLSL
#define JFX_MAIN_STEP_GLSL

#include "effects/lib/jfx_state.glsl"

#ifdef JFX_STEP_FORM
void main(){
    JfxStep s = jfx_step_context();
#ifdef JFX_SCATTER_FORM
    frag_color = jfx_scatter(s, jfx_placed());
#else
    frag_color = jfx_step(s);
#endif
}
#endif

#endif // JFX_MAIN_STEP_GLSL
