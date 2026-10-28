#version 450

#include "includes/htmlui_sdf.glsl"

// No sRGB conversion: RmlUi colors and textures are premultiplied sRGB and the engine's
// color targets store sRGB-encoded values, so blending happens in sRGB space like a browser.
// u_fs_htmluiClip (includes/htmlui_sdf.glsl, S16): the analytic clip and the texture's kind; a
// draw without either has every field 0 and its colour is the vertex colour times the texel.

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
    // derivatives first, in uniform control flow
    vec2 dpdx = dFdx(v_docPos);
    vec2 dpdy = dFdy(v_docPos);
    #ifdef HAS_TEXTURE
        vec4 texel = texture(sampler2D(u_htmluiTexture, u_htmlui_smp), v_uv1);
        vec2 uvTexels = v_uv1 * vec2(textureSize(sampler2D(u_htmluiTexture, u_htmlui_smp), 0));
        float distanceText = htmluiDistanceText(texel, uvTexels, v_docPos);
        if (clip.params.y > 1.5)
            color *= distanceText;
        else if (clip.params.y > 0.5)
            color *= texel.r;
        else
            color *= texel;
    #endif
    if (clip.params.x > 0.5)
        color *= htmluiClipCoverage(v_docPos, dpdx, dpdy);
    g_finalColor = color;
}
