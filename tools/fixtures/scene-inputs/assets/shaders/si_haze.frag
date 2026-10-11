#version 450

// @shader mesh
// @name "SI heat haze"
// @category "Fixture"
// @description "Scene-inputs fixture: the scene colour behind a quad, wobbled by scrolling sine waves and faded at the quad's border (sprites, 2D and 3D cameras)."
// @require uv1 scenecolor
// @state blend=alpha zwrite=off cull=off shadows=off
// @property amount float 0.012 "Distortion (screen UV)"
// @property frequency float 18 "Waves across the quad"

#define FRAME_FS
#include "includes/frame.glsl"

uniform u_fs_customParams {
    float amount;
    float frequency;
} customParams;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec2 edge = min(v_uv1, 1.0 - v_uv1);
    float mask = clamp(min(edge.x, edge.y) * 8.0, 0.0, 1.0);
    float t = frame.time.x * 3.0;
    vec2 wobble = vec2(sin(v_uv1.y * customParams.frequency + t), cos(v_uv1.x * customParams.frequency * 0.7 + t * 1.3));
    vec3 color = sceneColor(screenUV() + wobble * customParams.amount * mask);
    g_finalColor = finalizeColor(vec4(color, mask));
}
