#version 450

#include "includes/htmlui_sdf.glsl"

// No sRGB conversion: RmlUi colors and textures are premultiplied sRGB and the engine's
// color targets store sRGB-encoded values, so blending happens in sRGB space like a browser.

// S16: the analytic clip (R6) and the texture's kind
uniform u_fs_htmluiClip {
    vec4 rect[2];       // x, y, width, height: document px before the element transform
    vec4 radiiX[2];     // top-left, top-right, bottom-right, bottom-left
    vec4 radiiY[2];
    vec4 params;        // x: clip shapes (0, 1 or 2), y: 1 = a coverage texture (R8: .r tints every channel)
} clip;

in vec4 v_color;
#ifdef HAS_TEXTURE
    in vec2 v_uv1;
    uniform texture2D u_htmluiTexture;
    uniform sampler u_htmlui_smp;
#endif
in vec2 v_docPos;
out vec4 g_finalColor;

void main() {
    vec4 color = v_color;
    #ifdef HAS_TEXTURE
        vec4 texel = texture(sampler2D(u_htmluiTexture, u_htmlui_smp), v_uv1);
        color *= (clip.params.y > 0.5) ? texel.rrrr : texel;
    #endif
    if (clip.params.x > 0.5){
        vec2 dpdx = dFdx(v_docPos);
        vec2 dpdy = dFdy(v_docPos);
        float coverage = htmluiClipShape(v_docPos, dpdx, dpdy, clip.rect[0], clip.radiiX[0], clip.radiiY[0]);
        if (clip.params.x > 1.5)
            coverage *= htmluiClipShape(v_docPos, dpdx, dpdy, clip.rect[1], clip.radiiX[1], clip.radiiY[1]);
        color *= coverage;
    }
    g_finalColor = color;
}
