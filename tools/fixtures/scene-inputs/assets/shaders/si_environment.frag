#version 450

// @shader mesh
// @name "SI environment"
// @category "Fixture"
// @description "Scene-inputs fixture: a mirror ball reflecting the engine's reflection environment (includes/environment.glsl): the sky's prefiltered map, or the reflection probe around it; magenta where there is none."
// @require normals position environment
// @property roughness range(0, 1) 0 "Roughness"

uniform u_fs_customParams {
    float roughness;
} customParams;

in vec3 v_normal;
in vec3 v_position;

out vec4 g_finalColor;

#define FRAME_FS
#include "includes/frame.glsl"
#include "includes/environment.glsl"
#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    vec3 v = normalize(frame.cameraPosition.xyz - v_position);
    vec3 color = environmentPresent() ? environmentColor(reflect(-v, n), customParams.roughness) : vec3(1.0, 0.0, 1.0);
    g_finalColor = finalizeColor(vec4(color, 1.0));
}
