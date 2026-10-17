#version 450

// @shader points
// @name "SI soft points"
// @category "Fixture"
// @description "Scene-inputs fixture: round point sprites that fade where they meet opaque geometry (soft particles: scene depth minus fragment depth over a softness)."
// @require scenedepth
// @state zwrite=off
// @property tint color (1, 0.75, 0.3, 1) "Tint"
// @property softness float 0.6 "Softness (world units)"

uniform u_fs_customParams {
    vec4 tint;
    float softness;
} customParams;

out vec4 g_finalColor;

#include "includes/points_fs.glsl"
#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec2 c = pointCoord() * 2.0 - 1.0;
    float disc = clamp(1.0 - dot(c, c), 0.0, 1.0);
    vec4 particle = pointVertexColor();
    float soft = clamp((sceneLinearDepth(screenUV()) - fragmentLinearDepth()) / max(customParams.softness, 0.0001), 0.0, 1.0);
    vec4 color = vec4(customParams.tint.rgb * particle.rgb, disc * particle.a * soft);
    g_finalColor = finalizeStraightColor(color);
}
