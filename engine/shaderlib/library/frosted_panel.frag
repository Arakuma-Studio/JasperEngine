#version 450

// @shader ui
// @name "Frosted Panel"
// @category "2D"
// @description "A panel of frosted glass: the blurred scene behind it, tinted. Needs the scene below to have post-processing Blur on, and the panel in a scene drawn after it (a HUD layer scene); elsewhere it shows its tint."
// @property tintColor color (0.85, 0.9, 1, 1) "Tint colour"
// @property tintAmount range(0, 1) 0.2 "Tint amount"
// @property brightness range(0, 2) 1.1 "Brightness of the scene behind"

// The element's colour and texture (an Image's) shape and colour the panel: its alpha is the
// panel's, its colour multiplies the frosted background (white: the background as it is).

uniform u_fs_customParams {
    vec4 tintColor;
    float tintAmount;
    float brightness;
} customParams;

out vec4 g_finalColor;

#include "includes/ui_fs.glsl"
#include "includes/blurred_scene.glsl"
#include "includes/output.glsl"

void main() {
    vec4 element = uiBaseColor();
    vec3 background = blurredScene(blurredSceneUV()) * customParams.brightness;
    // no blurred scene (no blur, or the panel in the blurred scene itself): the tint alone
    float tint = mix(1.0, customParams.tintAmount, blurredSceneAvailable());
    vec3 color = mix(background, customParams.tintColor.rgb, tint) * element.rgb;
    g_finalColor = finalizeColor(vec4(color, element.a));
}
