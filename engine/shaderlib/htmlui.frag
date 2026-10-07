#version 450

// No sRGB conversion: RmlUi colors and textures are premultiplied sRGB and the engine's
// color targets store sRGB-encoded values, so blending happens in sRGB space like a browser.
in vec4 v_color;
#ifdef HAS_TEXTURE
    in vec2 v_uv1;
    uniform texture2D u_htmluiTexture;
    uniform sampler u_htmlui_smp;
#endif
out vec4 g_finalColor;

void main() {
    vec4 color = v_color;
    #ifdef HAS_TEXTURE
        color *= texture(sampler2D(u_htmluiTexture, u_htmlui_smp), v_uv1);
    #endif
    g_finalColor = color;
}
