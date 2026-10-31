#version 450

// A fork may declare a u_vs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Two names are
// written by the engine instead: "time" (seconds since startup) and "resolution"
// (xy = render target size, zw = 1 / size). Do not mix int and float members: GL
// uploads the block typed after its first member.
//
//   uniform u_vs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;
//
// The hairline (a Line Renderer without a width) and the ribbon of thick lines and trails
// (LINE_RIBBON, LINE_PIXEL_WIDTH) are includes/lines_vs.glsl: a fork that keeps this include
// draws both.

#include "includes/lines_vs.glsl"
