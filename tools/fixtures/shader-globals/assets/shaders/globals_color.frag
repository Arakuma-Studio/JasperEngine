#version 450

// @shader mesh
// @name "Global colour"
// @description "Shader globals fixture: draws the TestColor global (Project Settings > Graphics > Shader Globals, Shader.setGlobalColor)."

uniform u_fs_globals {
    vec4 TestColor;
} globals;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    g_finalColor = finalizeColor(globals.TestColor);
}
