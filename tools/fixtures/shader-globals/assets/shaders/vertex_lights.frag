#version 450

// @shader mesh
// @name "Vertex lights"
// @description "Shader globals fixture: lit per vertex by four point lights given as global shader values (vertex_lights.vert)."

in vec3 v_light;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    g_finalColor = finalizeColor(vec4(v_light, 1.0));
}
