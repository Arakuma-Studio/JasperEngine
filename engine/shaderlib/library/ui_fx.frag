#version 450

// @shader ui
// @name "UI Effects"
// @category "2D"
// @description "Image and Text effects: grayscale, hue shift, tint overlay and a moving diagonal shine."
// @feature GRAYSCALE "Grayscale"
// @feature HUE_SHIFT "Hue shift"
// @feature TINT_OVERLAY "Tint overlay"
// @feature SHINE "Shine"
// @property grayscaleAmount range(0, 1) 1 "Grayscale amount" feature=GRAYSCALE
// @property hueShift range(0, 360) 120 "Hue shift (degrees)" feature=HUE_SHIFT
// @property hueSpeed float 0 "Hue cycle speed (degrees per second)" feature=HUE_SHIFT
// @property tintColor color (1, 0.4, 0.2, 1) "Tint colour (overlay blend, keeps the shading)" feature=TINT_OVERLAY
// @property tintAmount range(0, 1) 0.75 "Tint amount" feature=TINT_OVERLAY
// @property shineColor hdrcolor (1, 1, 1, 0.8) "Shine colour (alpha: strength)" feature=SHINE
// @property shineWidth float 40 "Shine width (UI pixels)" feature=SHINE
// @property shineAngle range(-80, 80) 25 "Shine angle (degrees from vertical)" feature=SHINE
// @property shineSpeed float 600 "Shine speed (UI pixels per second)" feature=SHINE
// @property shineInterval float 2.5 "Shine interval (seconds between sweeps)" feature=SHINE
// @property shineSoftness range(0, 1) 0.6 "Shine edge softness" feature=SHINE

// The element's colour (texture or font atlas, colour, vertex colour) through grayscale,
// hue shift and tint overlay; the shine adds light inside the element's alpha. The shine
// band sweeps the element's own space in UI pixels (from its origin, at shineSpeed), once
// per interval, so it works on Images and Text alike whatever their texture.

#if defined(GRAYSCALE) || defined(HUE_SHIFT) || defined(TINT_OVERLAY) || defined(SHINE)
uniform u_fs_customParams {
    float time; // engine-written seconds
    #ifdef GRAYSCALE
        float grayscaleAmount;
    #endif
    #ifdef HUE_SHIFT
        float hueShift;
        float hueSpeed;
    #endif
    #ifdef TINT_OVERLAY
        vec4 tintColor;
        float tintAmount;
    #endif
    #ifdef SHINE
        vec4 shineColor;
        float shineWidth;
        float shineAngle;
        float shineSpeed;
        float shineInterval;
        float shineSoftness;
    #endif
} customParams;
#endif

out vec4 g_finalColor;

#include "includes/ui_fs.glsl"

// after ui_fs.glsl's inputs, in ui_vs.glsl's order
#ifdef SHINE
    in vec2 v_localPosition;
#endif
#include "includes/color_fx.glsl"
#include "includes/output.glsl"

void main() {
    vec4 color = uiBaseColor();

    #ifdef GRAYSCALE
        color.rgb = grayscale(color.rgb, customParams.grayscaleAmount);
    #endif

    #ifdef HUE_SHIFT
        // wrapped to a turn, so the rotation's sine and cosine stay precise in a long run
        color.rgb = hueRotate(color.rgb, radians(mod(customParams.hueShift + customParams.hueSpeed * customParams.time, 360.0)));
    #endif

    #ifdef TINT_OVERLAY
        // overlay blend (on the element's display values): the tint colours the mid
        // tones and keeps highlights and shadows
        vec3 base = linearTosRGB(color.rgb);
        vec3 tinted = sRGBToLinear(hardLight(linearTosRGB(customParams.tintColor.rgb), base));
        color.rgb = mix(color.rgb, tinted, customParams.tintAmount * customParams.tintColor.a);
    #endif

    #ifdef SHINE
        float angle = radians(customParams.shineAngle);
        float across = dot(v_localPosition, vec2(cos(angle), sin(angle)));
        float halfWidth = max(customParams.shineWidth * 0.5, 0.001);
        // the band starts just before the origin and moves on for the whole interval
        float center = mod(customParams.time, max(customParams.shineInterval, 0.01)) * customParams.shineSpeed - halfWidth;
        float inner = min(halfWidth * (1.0 - customParams.shineSoftness), halfWidth * 0.999);
        float band = 1.0 - smoothstep(inner, halfWidth, abs(across - center));
        color.rgb += customParams.shineColor.rgb * (customParams.shineColor.a * band);
    #endif

    g_finalColor = finalizeColor(color);
}
