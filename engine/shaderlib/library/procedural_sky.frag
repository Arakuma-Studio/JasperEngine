#version 450

// @shader sky
// @name "Procedural Sky"
// @category "Sky"
// @description "Sky, horizon and ground colours with a sun disc and halo at the main directional light, optional scrolling clouds."
// @property skyColor color (0.24, 0.47, 0.86, 1) "Sky colour (zenith)"
// @property horizonColor color (0.8, 0.87, 0.95, 1) "Horizon colour"
// @property groundColor color (0.37, 0.35, 0.33, 1) "Ground colour"
// @property skyExponent range(0.05, 8) 0.5 "Sky exponent (lower: the sky colour reaches further down)"
// @property groundBlend range(0.001, 1) 0.06 "Horizon to ground blend height"
// @property sunSize range(0.05, 10) 1.6 "Sun disc radius (degrees)"
// @property sunSharpness range(0, 1) 0.85 "Sun disc edge sharpness"
// @property sunIntensity range(0, 20) 6 "Sun disc intensity"
// @property haloIntensity range(0, 4) 0.6 "Halo intensity"
// @property haloExponent range(1, 1024) 32 "Halo tightness (exponent)"
// @property exposure range(0, 8) 1 "Exposure"
// @property cloudColor color (1, 1, 1, 1) "Cloud colour (alpha: opacity)" feature=CLOUDS
// @property cloudCoverage range(0, 1) 0.5 "Cloud coverage" feature=CLOUDS
// @property cloudSoftness range(0.01, 1) 0.3 "Cloud edge softness" feature=CLOUDS
// @property cloudScale float 0.35 "Cloud scale (texture repeats per unit of the cloud plane)" feature=CLOUDS
// @property cloudSpeed vector4 (0.01, 0.004, -0.006, 0.009) "Cloud scroll speed, layer 1 (xy) and layer 2 (zw)" feature=CLOUDS
// @texture u_cloudTexture gray "Cloud noise (tiling, red channel)" feature=CLOUDS
// @feature CLOUDS "Clouds"

// The sun is the frame block's main light (the brightest directional light): its
// direction places the disc and halo, its colour tints them; without one there is no sun.
// Directions are in world space (SKY_WORLD_DIRECTION), so the Skybox's rotation moves neither sun nor clouds.

#define SKY_WORLD_DIRECTION // as procedural_sky.vert
#define FRAME_FS
#include "includes/frame.glsl"

uniform u_fs_customParams {
    float time; // engine-written seconds
    vec4 skyColor;
    vec4 horizonColor;
    vec4 groundColor;
    float skyExponent;
    float groundBlend;
    float sunSize;
    float sunSharpness;
    float sunIntensity;
    float haloIntensity;
    float haloExponent;
    float exposure;
    #ifdef CLOUDS
        vec4 cloudColor;
        float cloudCoverage;
        float cloudSoftness;
        float cloudScale;
        vec4 cloudSpeed;
    #endif
} customParams;

#ifdef CLOUDS
    uniform texture2D u_cloudTexture;
    uniform sampler u_cloud_smp;
#endif

in vec3 v_worldDirection; // SKY_WORLD_DIRECTION (procedural_sky.vert)

out vec4 frag_color;

#include "includes/output.glsl"

void main(){
    vec3 direction = normalize(v_worldDirection);
    float height = direction.y;

    // gradient: zenith -> horizon above, horizon -> ground below
    float skyAmount = pow(clamp(height, 0.0, 1.0), customParams.skyExponent);
    float groundAmount = smoothstep(0.0, customParams.groundBlend, -height);
    vec3 color = mix(customParams.horizonColor.rgb, customParams.skyColor.rgb, skyAmount);
    color = mix(color, customParams.groundColor.rgb, groundAmount);

    // sun disc and halo, hidden by the ground
    vec3 toSun = frame.mainLightDirection.xyz;
    float hasSun = frame.mainLightDirection.w > 0.0 ? 1.0 : 0.0;
    float cosAngle = dot(direction, toSun);
    float above = 1.0 - groundAmount;
    vec3 sunColor = frame.mainLightColor.rgb * hasSun;

    float halo = pow(max(cosAngle, 0.0), customParams.haloExponent) * customParams.haloIntensity;
    color += sunColor * halo * mix(0.35, 1.0, above);

    float outer = radians(customParams.sunSize);
    float inner = outer * min(customParams.sunSharpness, 0.995);
    float disc = smoothstep(cos(outer), cos(inner), cosAngle) * above;

    #ifdef CLOUDS
        // two noise layers on a plane above the camera, scrolling apart; thinned towards
        // the horizon where the plane's texture compresses
        vec2 plane = direction.xz / max(height, 0.08) * customParams.cloudScale;
        vec2 offset1 = customParams.cloudSpeed.xy * customParams.time;
        vec2 offset2 = customParams.cloudSpeed.zw * customParams.time;
        float layer1 = texture(sampler2D(u_cloudTexture, u_cloud_smp), plane + offset1).r;
        float layer2 = texture(sampler2D(u_cloudTexture, u_cloud_smp), plane * 2.3 + vec2(0.37, 0.71) + offset2).r;
        float density = layer1 * 0.65 + layer2 * 0.35;
        float threshold = 1.0 - customParams.cloudCoverage;
        float cloud = smoothstep(threshold, threshold + customParams.cloudSoftness, density);
        cloud *= smoothstep(0.0, 0.12, height) * customParams.cloudColor.a;
        // lit by the sun, brighter around it; the clouds also cover the disc
        vec3 cloudLit = customParams.cloudColor.rgb * (mix(customParams.horizonColor.rgb, vec3(1.0), 0.6) + sunColor * halo * 2.0);
        color = mix(color, cloudLit, cloud);
        disc *= 1.0 - cloud;
    #endif

    color += sunColor * disc * customParams.sunIntensity;

    frag_color = finalizeColor(vec4(color * customParams.exposure, 1.0));
}
