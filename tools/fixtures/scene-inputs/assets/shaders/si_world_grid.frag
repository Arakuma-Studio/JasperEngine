#version 450

// @shader mesh
// @name "SI world grid"
// @category "Fixture"
// @description "Scene-inputs fixture: an opaque surface with white lines on the integer world grid (its own world position), the reference for si_depth_grid."
// @require position
// @state blend=opaque
// @property baseColor color (0.32, 0.36, 0.42, 1) "Base colour"
// @property lineWidth float 0.07 "Half line width (world units)"

uniform u_fs_customParams {
    vec4 baseColor;
    float lineWidth;
} customParams;

in vec3 v_position;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec3 d = abs(fract(v_position + 0.5) - 0.5);
    float lineDistance = min(d.x, d.z);
    vec3 color = lineDistance < customParams.lineWidth ? vec3(1.0) : customParams.baseColor.rgb;
    // a shade from the height, so the uneven ground reads as such
    color *= 0.75 + 0.25 * clamp(v_position.y * 0.5 + 0.5, 0.0, 1.0);
    g_finalColor = finalizeColor(vec4(color, 1.0));
}
