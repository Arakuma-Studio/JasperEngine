#version 450

// @shader mesh
// @name "Matcap"
// @category "Stylized"
// @description "Material capture: the colour comes from a matcap image looked up by the view-space normal, replacing, multiplying or adding to the base map. Assign a matcap image; unlit."
// @require position normals uv1 color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property matcapGain float 1 "Matcap gain"
// @property matcapPower float 1 "Matcap power"
// @property rimColor hdrcolor (1, 1, 1, 1) "Rim colour" feature=RIM
// @property rimPower range(0.5, 16) 4 "Rim power" feature=RIM
// @texture u_matcapTexture white "Matcap"
// @texture u_baseTexture white "Base map"
// @keywords MATCAP_MODE "Mode" REPLACE MULTIPLY ADD
// @feature RIM "Rim light"

// m = pow(matcap * matcapGain, matcapPower) (gain 1 and power 1 leave the image as is),
// tint = the tint times the instance colour:
//     REPLACE   tint * m                      the matcap is the shading and the colour
//     MULTIPLY  tint * base map * m           the matcap shades the base map
//     ADD       tint * base map + m           the matcap adds shine (a black matcap adds nothing)
// RIM adds rimColor * pow(1 - n.v, rimPower). The lookup corrects the perspective
// stretch at the screen's edges (includes/matcap.glsl). With no matcap assigned the
// default is white: REPLACE then draws the flat tint.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 rimColor;
    float matcapGain;
    float matcapPower;
    float rimPower;
} customParams;

uniform texture2D u_matcapTexture;
uniform sampler u_matcap_smp;

#ifndef MATCAP_MODE_REPLACE
    uniform texture2D u_baseTexture;
    uniform sampler u_base_smp;

    in vec2 v_uv1;
#endif

in vec3 v_position;
in vec3 v_normal;
in vec4 v_color; // the instance colour

out vec4 g_finalColor;

#include "includes/matcap.glsl"
#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing){
        n = -n;
    }

    vec3 matcap = sRGBToLinear(texture(sampler2D(u_matcapTexture, u_matcap_smp), matcapUV(n, v_position)).rgb);
    matcap = pow(matcap * customParams.matcapGain, vec3(customParams.matcapPower));

    vec4 tint = customParams.baseColor * v_color;
    #ifdef MATCAP_MODE_REPLACE
        vec4 color = vec4(tint.rgb * matcap, tint.a);
    #else
        vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
        vec4 base = tint * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv));
        #ifdef MATCAP_MODE_MULTIPLY
            vec4 color = vec4(base.rgb * matcap, base.a);
        #else
            vec4 color = vec4(base.rgb + matcap, base.a);
        #endif
    #endif

    #ifdef RIM
        vec3 toCamera = (frame.cameraPosition.w > 0.5) ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
        color.rgb += customParams.rimColor.rgb * pow(max(1.0 - dot(n, toCamera), 0.0), customParams.rimPower);
    #endif

    g_finalColor = finalizeColor(color);
}
