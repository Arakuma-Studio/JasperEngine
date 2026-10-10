#version 450

// @shader sky
// @name "Gradient Sky"
// @category "Sky"
// @description "Top, horizon and bottom colours blended by the view direction's height, each side with its own exponent."
// @property topColor color (0.2, 0.45, 0.85, 1) "Top colour"
// @property horizonColor color (0.85, 0.9, 0.96, 1) "Horizon colour"
// @property bottomColor color (0.36, 0.34, 0.32, 1) "Bottom colour"
// @property topExponent range(0.05, 8) 0.6 "Top exponent (lower: the top colour reaches further down)"
// @property bottomExponent range(0.05, 8) 0.4 "Bottom exponent (lower: the bottom colour reaches further up)"
// @property horizonOffset range(-1, 1) 0 "Horizon offset (height of the horizon line)"
// @property intensity range(0, 4) 1 "Intensity"

uniform u_fs_customParams {
    vec4 topColor;
    vec4 horizonColor;
    vec4 bottomColor;
    float topExponent;
    float bottomExponent;
    float horizonOffset;
    float intensity;
} customParams;

in vec3 v_direction;

out vec4 frag_color;

#include "includes/output.glsl"

void main(){
    float height = normalize(v_direction).y - customParams.horizonOffset;
    // the share of the top (above the horizon) or bottom colour, 0 at the horizon
    float top = pow(clamp(height / max(1.0 - customParams.horizonOffset, 1e-4), 0.0, 1.0), customParams.topExponent);
    float bottom = pow(clamp(-height / max(1.0 + customParams.horizonOffset, 1e-4), 0.0, 1.0), customParams.bottomExponent);

    vec3 horizon = customParams.horizonColor.rgb;
    vec3 color = horizon + (customParams.topColor.rgb - horizon) * top + (customParams.bottomColor.rgb - horizon) * bottom;

    frag_color = finalizeColor(vec4(color * customParams.intensity, 1.0));
}
