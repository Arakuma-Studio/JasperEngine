#version 450

// @shader mesh
// @name "SI refraction"
// @category "Fixture"
// @description "Scene-inputs fixture: glass showing the opaque scene behind it (sceneColor) offset by its normal; samples that would land on something in front of the glass fall back to the straight view (sceneLinearDepth)."
// @require normals scenecolor scenedepth
// @state blend=alpha zwrite=off shadows=off
// @property tint color (0.85, 0.95, 1, 1) "Tint"
// @property strength float 0.05 "Offset (screen UV per unit of normal)"
// @property rim float 0.25 "Rim brightness"

// With strength 0, tint white and rim 0 it is invisible: the copy of the scene at the
// fragment's own screen UV is what is behind it (full-resolution copy: pixel for pixel).

uniform u_fs_customParams {
    vec4 tint;
    float strength;
    float rim;
} customParams;

in vec3 v_normal;

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    vec2 uv = screenUV();
    vec2 refracted = uv + n.xy * customParams.strength;
    // something in front of the glass is not behind it: keep the straight view there
    if (sceneLinearDepth(refracted) < fragmentLinearDepth()){
        refracted = uv;
    }
    // the scene colour has its fog: the tint (a filter) fades to white and the rim (added
    // light) to black with it, and the result is written without fogging it again
    vec3 color = sceneColor(refracted) * fadeByFog(customParams.tint.rgb, vec3(1.0));
    color += fadeByFog(customParams.rim * (1.0 - abs(n.z)) * vec3(0.6, 0.8, 1.0), vec3(0.0));
    g_finalColor = finalizeFoggedColor(vec4(color, 1.0));
}
