#version 450

// @shader mesh
// @name "SI intersection glow"
// @category "Fixture"
// @description "Scene-inputs fixture: a force-field glow where the surface meets opaque geometry (scene depth minus fragment depth), plus a faint fill."
// @require scenedepth
// @state blend=additive zwrite=off cull=off shadows=off
// @property glowColor hdrcolor (0.3, 1.0, 1.6, 1) "Glow colour"
// @property width float 0.35 "Intersection band (world units)"
// @property fill float 0.01 "Fill"

// screenUV, sceneLinearDepth and fragmentLinearDepth: the band is bright where the opaque
// surface behind is within `width` world units of this fragment, on every backend and in
// perspective and orthographic cameras alike (both depths are view-space distances).

uniform u_fs_customParams {
    vec4 glowColor;
    float width;
    float fill;
} customParams;

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec2 uv = screenUV();
    float gap = sceneLinearDepth(uv) - fragmentLinearDepth();
    float band = 1.0 - clamp(gap / max(customParams.width, 0.0001), 0.0, 1.0);
    g_finalColor = finalizeColor(vec4(customParams.glowColor.rgb * (band * band + customParams.fill), 1.0));
}
