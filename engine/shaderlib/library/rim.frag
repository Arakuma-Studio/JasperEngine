#version 450

// @shader mesh
// @name "Rim"
// @category "Effects"
// @description "Force field and fresnel glow: bright where the surface turns away from the view, optionally with a scrolling pattern. Additive, no depth write, no shadows; draw both faces (cull off) for a fuller shield."
// @require normals position
// @require uv1 feature=TEXTURE
// @state blend=additive zwrite=off cull=back shadows=off
// @property rimColor hdrcolor (0.3, 0.75, 1, 1) "Rim colour"
// @property power range(0.25, 8) 2.5 "Rim power"
// @property intensity float 2 "Rim intensity"
// @property fill range(0, 1) 0.06 "Fill (glow facing the view)"
// @property textureIntensity float 0.6 "Pattern intensity" feature=TEXTURE
// @property textureTiling vector4 (4, 2, 0, 0) "Pattern tiling (xy) and offset (zw)" feature=TEXTURE
// @property textureSpeed vector2 (0.05, 0.1) "Pattern scroll (UV per second)" feature=TEXTURE
// @texture u_patternTexture white "Pattern" feature=TEXTURE
// @feature TEXTURE "Scrolling pattern"

// glow = pow(1 - |n.v|, power) * intensity + fill, times rimColor; abs() makes the back
// faces glow at their edges too. TEXTURE adds a pattern scrolling over the UVs, brightest
// where the rim is. Made for the additive blend (black adds nothing; the fog fades it out);
// alpha is the colour's alpha, which scales what the blend adds. No shadows: a glow casts
// none. Only the pattern reads the UVs (@require uv1 feature=TEXTURE).

uniform u_fs_customParams {
    vec4 rimColor;
    vec4 textureTiling;
    vec2 textureSpeed;
    float power;
    float intensity;
    float fill;
    float textureIntensity;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

#ifdef TEXTURE
    uniform texture2D u_patternTexture;
    uniform sampler u_pattern_smp;

    in vec2 v_uv1;
#endif

in vec3 v_position;
in vec3 v_normal;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    // towards the camera (an orthographic camera looks along one direction)
    vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    // max: |n.v| of two unit vectors can round above 1, and pow of a negative is NaN
    float rim = pow(max(1.0 - abs(dot(n, v)), 0.0), customParams.power);
    float glow = rim * customParams.intensity + customParams.fill;

    #ifdef TEXTURE
        // the scroll wraps every UV unit, so a long run keeps its precision
        vec2 scroll = fract(frame.time.x * customParams.textureSpeed);
        vec2 uv = v_uv1 * customParams.textureTiling.xy + customParams.textureTiling.zw + scroll;
        vec3 pattern = sRGBToLinear(texture(sampler2D(u_patternTexture, u_pattern_smp), uv).rgb);
        vec3 color = customParams.rimColor.rgb * (glow + pattern * customParams.textureIntensity * (0.25 + rim));
    #else
        vec3 color = customParams.rimColor.rgb * glow;
    #endif

    g_finalColor = finalizeStraightColor(vec4(color, customParams.rimColor.a));
}
