// HTML UI effects: main() of a paint effect (@kind paint), included last by the effect file
// after its vec4 jfx_paint(JfxContext c) (straight-alpha linear sRGB).
//  - paint form: the paint over the element's geometry, premultiplied, times the vertex alpha
//    (the element's draw opacity), the opacity parameter and, for @geometry fill, the coverage
//    of the area's rounded box (frame.misc.w); the blend parameter is the pipeline's
//    fixed-function blend.
//  - filter form (filter, backdrop-filter, the gradient-text fill): the paint recolours the
//    content inside its alpha with the blend parameter, then mixes with it by opacity.
#ifndef JFX_MAIN_PAINT_GLSL
#define JFX_MAIN_PAINT_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sdf.glsl"

// (the step forms of a stateful effect have main_step.glsl's main)
#ifndef JFX_STEP_FORM
void main(){
    JfxContext c = jfx_context();
#ifdef JFX_FILTER_FORM
    vec4 src = texture(sampler2D(u_effectSource, u_effectSource_smp), v_texcoord);
    vec4 S = jfx_unpremultiply(src);                   // sRGB, straight
    vec4 encoded = jfx_encode(jfx_paint(c));
    float pa = encoded.a;
    vec3 P = pa > 1e-5 ? encoded.rgb / pa : vec3(0.0);  // the paint in sRGB, straight
    float blend = frame.misc.y;
    vec3 rgb;
    if (blend < 0.5)
        rgb = mix(S.rgb, P, pa);                        // normal
    else if (blend < 1.5)
        rgb = S.rgb + P * pa;                           // add
    else if (blend < 2.5)
        rgb = 1.0 - (1.0 - S.rgb) * (1.0 - P * pa);     // screen
    else
        rgb = S.rgb * mix(vec3(1.0), P, pa);            // multiply
    vec4 result = vec4(clamp(rgb, 0.0, 1.0) * S.a, S.a);
    frag_color = mix(src, result, clamp(frame.misc.x, 0.0, 1.0));
#else
    // misc.w: clipped to the area's rounded box (@geometry fill; bounds geometry draws beyond it)
    float coverage = frame.misc.w > 0.5 ? jfx_aa_fill(jfx_box_sdf(c.px, c.size, c.radii)) : 1.0;
    frag_color = jfx_encode(jfx_paint(c)) * (v_color.a * clamp(frame.misc.x, 0.0, 1.0) * coverage);
#endif
}
#endif // JFX_STEP_FORM

#endif // JFX_MAIN_PAINT_GLSL
