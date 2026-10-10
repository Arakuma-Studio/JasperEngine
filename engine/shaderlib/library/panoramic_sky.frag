#version 450

// @shader sky
// @name "Panoramic Sky"
// @category "Sky"
// @description "An equirectangular (latitude-longitude) 2D image as the sky, with rotation, exposure and tint."
// @property tint color (1, 1, 1, 1) "Tint"
// @property exposure range(0, 8) 1 "Exposure"
// @property rotation range(0, 360) 0 "Rotation (degrees about the vertical)"
// @texture u_panoramaTexture gray "Panorama (equirectangular, 2:1)"

// The image's horizontal centre faces -Z (the default camera's forward); its top row is
// straight up. The Skybox's own rotation applies too.

uniform u_fs_customParams {
    vec4 tint;
    float exposure;
    float rotation;
} customParams;

uniform texture2D u_panoramaTexture;
uniform sampler u_panorama_smp;

in vec3 v_direction;

out vec4 frag_color;

#include "includes/output.glsl"

const float INV_TWO_PI = 0.15915494309189535;
const float INV_PI = 0.3183098861837907;

void main(){
    vec3 direction = normalize(v_direction);

    // longitude in turns, 0.5 at -Z; latitude 0 at the top row
    float longitude = atan(direction.x, -direction.z) * INV_TWO_PI + 0.5 - customParams.rotation / 360.0;
    float latitude = acos(clamp(direction.y, -1.0, 1.0)) * INV_PI;

    // The longitude wraps from 1 to 0 across one column of pixels, where its screen
    // derivative jumps by a whole turn and the sampler would pick the smallest mip: a
    // visible seam. The same longitude wrapped half a turn away is continuous there, so
    // the gradient is taken from whichever of the two changes least (per axis).
    float u1 = fract(longitude);
    float u2 = fract(longitude + 0.5) - 0.5;
    float dudx = abs(dFdx(u1)) <= abs(dFdx(u2)) ? dFdx(u1) : dFdx(u2);
    float dudy = abs(dFdy(u1)) <= abs(dFdy(u2)) ? dFdy(u1) : dFdy(u2);
    vec2 uv = vec2(u1, latitude);

    vec4 texel = textureGrad(sampler2D(u_panoramaTexture, u_panorama_smp), uv,
                             vec2(dudx, dFdx(latitude)), vec2(dudy, dFdy(latitude)));

    vec3 color = sRGBToLinear(texel.rgb) * customParams.tint.rgb * customParams.exposure;
    frag_color = finalizeColor(vec4(color, 1.0));
}
