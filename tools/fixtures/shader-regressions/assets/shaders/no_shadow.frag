#version 450
// @shader mesh
// @name "No shadow"
// @description "A flat colour that casts no shadow (@state shadows=off), and an additive look while GLOW is on (@state[GLOW])."
// @require uv1
// @state blend=opaque shadows=off
// @state[GLOW] blend=additive zwrite=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @feature GLOW "Glow"

#include "includes/output.glsl"

uniform u_fs_customParams {
    vec4 baseColor;
} customParams;

in vec2 v_uv1;
out vec4 g_finalColor;

void main(){
    vec4 color = customParams.baseColor * vec4(0.75 + 0.25 * step(0.5, fract(v_uv1.x * 4.0)), 1.0, 1.0, 1.0);
    #ifdef BLEND_ADDITIVE
        // the keyword state turned the blend additive: a dimmer glow
        color.rgb *= 0.6;
    #endif
    g_finalColor = finalizeColor(color);
}
