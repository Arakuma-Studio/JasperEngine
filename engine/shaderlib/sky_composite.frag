#version 450

// Atmosphere/SkyComposite: drawn in the colour pass after the opaque queue, a fullscreen
// triangle at the far plane with depth test LESS_EQUAL and no depth write, so it covers the sky
// pixels only (and the cleared background where there is no sky). It lays the Sky Map clouds
// and the height fog's sky fill over the sky: the fill (the fog's light, up to the sky fill
// angle above the horizon) on the sky behind the clouds, the clouds (which carry their own
// aerial perspective) in front:
//   sky' = (sky (1 - fill) + fogLight fill) Tc + Sc
// No sky shader changes: every sky (cube map, library and fork skies) gets it.

in vec2 v_texcoord;
out vec4 frag_color;

// the Sky Map, the current refresh and the one before it (crossfaded by params.x)
uniform texture2D u_cloudsMap;
uniform sampler u_cloudsMap_smp;
uniform texture2D u_cloudsMapPrevious;
uniform sampler u_cloudsMapPrevious_smp;

uniform u_fs_skyComposite {
    vec4 params;   // x the crossfade (0 previous .. 1 current map), y 1: RGBA8 (compressed) maps,
                   // z the clouds' opacity factor (0: no clouds), w unused
} sky;

#define USE_POST_COLOR
#include "includes/output.glsl"
#include "includes/atmosphere.glsl"
#include "includes/height_fog.glsl"

void main(){
    vec3 nearPoint = heightFogWorldPosition(gl_FragCoord.xy, 0.0);
    vec3 farPoint = heightFogWorldPosition(gl_FragCoord.xy, 1.0);
    vec3 dir = normalize(farPoint - nearPoint);

    vec3 cloudScattering = vec3(0.0);
    float cloudTransmittance = 1.0;
    float cloudOpacity = sky.params.z;
    if (cloudOpacity > 0.0 && dir.y > 0.0){
        // AtmosphereMath::hemiOctahedralEncode
        vec2 oct = dir.xz / (abs(dir.x) + dir.y + abs(dir.z));
        vec2 uv = atmosphereTargetUV(vec2(oct.x + oct.y, oct.x - oct.y) * 0.5 + 0.5);
        vec4 current = textureLod(sampler2D(u_cloudsMap, u_cloudsMap_smp), uv, 0.0);
        vec4 previous = textureLod(sampler2D(u_cloudsMapPrevious, u_cloudsMapPrevious_smp), uv, 0.0);
        vec3 s0;
        float t0;
        vec3 s1;
        float t1;
        atmosphereUnpack(previous, sky.params.y, s0, t0);
        atmosphereUnpack(current, sky.params.y, s1, t1);
        cloudScattering = mix(s0, s1, sky.params.x) * cloudOpacity;
        cloudTransmittance = 1.0 - (1.0 - mix(t0, t1, sky.params.x)) * cloudOpacity;
    }

    float fill = heightFogSkyAmount(dir);
    vec3 fogLight = heightFogInscatter(dir, 1.0);
    vec3 scattering = fogLight * fill * cloudTransmittance + cloudScattering;
    float transmittance = (1.0 - fill) * cloudTransmittance;
    frag_color = atmosphereLayerColor(scattering, transmittance);
}
