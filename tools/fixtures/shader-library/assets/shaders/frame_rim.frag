#version 450

// @shader mesh
// @name "Frame rim"
// @description "A project fork with a manifest: rim light from the frame block's camera, main light and ambient."
// @require normals position
// @property rimColor color (0.3, 0.8, 1.0, 1.0) "Rim colour"
// @property power float 2.5 "Rim power"
// @property wobble float 0.04 "Wobble"
// @texture u_tintTexture white "Tint"
// @feature PULSE "Pulse with time"

#define FRAME_FS
#include "includes/frame.glsl"

uniform u_fs_customParams {
    vec4 rimColor;
    float power;
} customParams;

uniform texture2D u_tintTexture;
uniform sampler u_tint_smp;

in vec3 v_position;
in vec3 v_normal;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    vec3 toCamera = normalize(frame.cameraPosition.xyz - v_position);
    float rim = pow(1.0 - max(dot(n, toCamera), 0.0), customParams.power);
    float diffuse = max(dot(n, frame.mainLightDirection.xyz), 0.0) * frame.mainLightDirection.w;
    vec3 base = frame.ambient.rgb + frame.mainLightColor.rgb * diffuse * 0.25;
    vec3 tint = sRGBToLinear(texture(sampler2D(u_tintTexture, u_tint_smp), vec2(0.5)).rgb);

    float pulse = 1.0;
    #ifdef PULSE
        pulse = 0.6 + 0.4 * frame.time.y;
    #endif

    g_finalColor = finalizeColor(vec4((base + customParams.rimColor.rgb * rim * pulse) * tint, 1.0));
}
