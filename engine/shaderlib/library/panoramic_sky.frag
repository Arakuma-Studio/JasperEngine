#version 450

// @shader sky
// @name "Panoramic Sky"
// @category "Sky"
// @description "An equirectangular (latitude-longitude) 2D image as the sky, with rotation, exposure and tint. Unity's Skybox/Panoramic _Rotation r is rotation (r + 270) mod 360."
// @property tint color (1, 1, 1, 1) "Tint"
// @property exposure range(0, 8) 1 "Exposure"
// @property rotation range(0, 360) 0 "Rotation (degrees about the vertical)"
// @texture u_panoramaTexture gray "Panorama (equirectangular, 2:1)"
// @feature VERTEX_MAPPING "Per-Vertex Mapping"

// The image's horizontal centre faces -Z (the default camera's forward); its top row is
// straight up. The Skybox's own rotation applies too.
//
// Per-Vertex Mapping (VERTEX_MAPPING, mobile and web): the longitude and latitude come
// interpolated from the engine's 64 x 32 sphere grid (includes/sky_vs.glsl SKY_SPHERE_UV), so a
// pixel takes one textureLod read of the base level and no atan, acos or derivatives. It
// matches the per-pixel mapping within 3/255 but within about 6 degrees of the poles (the
// grid's fans), and does not filter down a panorama minified on screen (one wider than about
// 6 screen widths per turn: keep it off for those).

uniform u_fs_customParams {
    vec4 tint;
    float exposure;
    float rotation;
} customParams;

uniform texture2D u_panoramaTexture;
uniform sampler u_panorama_smp;

#ifdef VERTEX_MAPPING
    in vec2 v_skyUV;
#else
    in vec3 v_direction;
#endif

out vec4 frag_color;

#include "includes/output.glsl"

const float INV_TWO_PI = 0.15915494309189535;
const float INV_PI = 0.3183098861837907;

void main(){
#ifdef VERTEX_MAPPING
    // fract per pixel: the interpolated longitude is continuous across the seam column
    vec2 uv = vec2(fract(v_skyUV.x - customParams.rotation / 360.0), v_skyUV.y);
    vec4 texel = textureLod(sampler2D(u_panoramaTexture, u_panorama_smp), uv, 0.0);
#else
    vec3 direction = normalize(v_direction);

    // longitude in turns, 0.5 at -Z; latitude 0 at the top row. atan(0, 0) is undefined
    // (straight up or down, where any longitude is the same row): that pixel takes -Z's
    float alongZ = (abs(direction.x) + abs(direction.z) > 0.0) ? -direction.z : 1.0;
    float longitude = atan(direction.x, alongZ) * INV_TWO_PI + 0.5 - customParams.rotation / 360.0;
    float latitude = acos(clamp(direction.y, -1.0, 1.0)) * INV_PI;

    // The longitude wraps from 1 to 0 across one column of pixels, where its screen
    // derivative jumps by a whole turn and the sampler would pick the smallest mip: a
    // visible seam. The same longitude wrapped half a turn away is continuous there, so
    // the gradient is taken from whichever of the two changes least (per axis). The
    // derivatives are all taken first, in uniform control flow, then picked.
    float u1 = fract(longitude);
    float u2 = fract(longitude + 0.5) - 0.5;
    float du1dx = dFdx(u1);
    float du2dx = dFdx(u2);
    float du1dy = dFdy(u1);
    float du2dy = dFdy(u2);
    float dudx = abs(du1dx) <= abs(du2dx) ? du1dx : du2dx;
    float dudy = abs(du1dy) <= abs(du2dy) ? du1dy : du2dy;
    vec2 uv = vec2(u1, latitude);

    vec4 texel = textureGrad(sampler2D(u_panoramaTexture, u_panorama_smp), uv,
                             vec2(dudx, dFdx(latitude)), vec2(dudy, dFdy(latitude)));
#endif

    vec3 color = sRGBToLinear(texel.rgb) * customParams.tint.rgb * customParams.exposure;
    frag_color = finalizeColor(vec4(color, 1.0));
}
