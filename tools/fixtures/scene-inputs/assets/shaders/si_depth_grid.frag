#version 450

// @shader mesh
// @name "SI depth grid"
// @category "Fixture"
// @description "Scene-inputs fixture: a box volume that draws, on the opaque surfaces inside it, thin red lines on the integer world grid reconstructed from the scene depth (sceneWorldPosition)."
// @require scenedepth
// @state blend=alpha zwrite=off ztest=always cull=front shadows=off
// @property lineColor color (1, 0.1, 0.05, 1) "Line colour"
// @property boxMin vector3 (-1, -1, -1) "Volume min (world)"
// @property boxMax vector3 (1, 1, 1) "Volume max (world)"
// @property lineWidth float 0.03 "Half line width (world units)"

// Drawn as the back faces of a box around the area, depth test off (they are behind the
// surfaces they shade), so every pixel the volume covers is shaded once, camera outside or
// inside. The red lines land inside the wider white lines
// si_world_grid draws on the same surfaces from their own world positions: any error of
// the reconstruction (orientation, depth decode, projection) moves them apart.

uniform u_fs_customParams {
    vec4 lineColor;
    vec3 boxMin;
    vec3 boxMax;
    float lineWidth;
} customParams;

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec3 p = sceneWorldPosition(screenUV());
    if (any(lessThan(p, customParams.boxMin)) || any(greaterThan(p, customParams.boxMax))){
        discard;
    }
    // distance to the nearest integer line, on the two axes of the box's largest face
    vec3 d = abs(fract(p + 0.5) - 0.5);
    float lineDistance = min(d.x, d.z);
    if (lineDistance > customParams.lineWidth){
        discard;
    }
    g_finalColor = finalizeColor(vec4(customParams.lineColor.rgb, 1.0));
}
