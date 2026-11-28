#version 450

// Atmosphere/VolumetricFog: the height fog's medium raymarched from the camera to the nearer of
// the scene's surface and the volumetric distance, at a reduced resolution, with what the
// analytic fog cannot have: the sun's shadow (light shafts: one shadow tap per step on Low, two
// rotated ones on Medium and High), a density noise, Local Volumetric Fog volumes and point and
// spot lights (1, 2 or 4 by tier), shadowed by one tap per step when they cast shadows and light
// shafts are on (docs/specs/S101-vapor.md R13-R15). SHADOW_DATA_POINT (the 'Pnt' variant) adds the
// point lights' shadow block and atlas, bound only when a shadowed point light is selected.
// Output (S, T) of that part of the ray; the composite puts it in place of the analytic fog the
// opaque surfaces took over the same part (volumetric_composite.frag), so the two never overlap.
//
// Steps are spread quadratically (denser near the camera) and offset by the static blue-noise
// jitter; each step's optical depth is the medium's closed form over the step plus the
// volumes' and noise's, and its light is integrated energy-conserving (S += T L (1 - e^-tau)).
// Without shadow, noise and volumes the steps telescope to L (1 - T) exactly: the image
// equals the analytic fog's (docs/specs/S12-atmosphere.md R6).
//
// The Lit colour mode (docs/specs/S101-vapor.md R12): the height fog block of this pass carries the
// light untinted (ambient as the fog colour, the sun as it is), and each step scatters it by
// the albedo of the media there, mixed by their share of the extinction; the fog's emissive is
// added by the global medium's share alone. A volume is so tinted by its own albedo only.

#define ATMOSPHERE_BLUE_NOISE
#define MAX_FOG_STEPS 64
#define MAX_FOG_VOLUMES 8
#define MAX_FOG_LIGHTS 4

in vec2 v_texcoord;
out vec4 frag_color;

// the reduced-resolution linear depth of this pass (atmosphere_depth.frag), same size
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
// the clouds' detail noise (32^3, repeat): the density noise of the fog and of the volumes
uniform texture3D u_fogNoise;
uniform sampler u_fogNoise_smp;

uniform u_fs_volumetricFog {
    vec4 target;              // xy 1 / target size, z steps, w volumetric distance
    vec4 camera;              // xyz camera world forward, w 1 orthographic (0 perspective)
    vec4 depthInfo;           // x far clip, y near clip, z 1: RGBA8 output (compressed), w sun shadow taps per step (0: no light shafts)
    vec4 noise;               // x intensity, y 1 / scale, z volumes, w lights
    vec4 noiseOffset;         // xyz wind offset (m)
    vec4 fogAlbedo;           // rgb the global medium's albedo (linear), w 1: Lit colour mode (else rgb 1)
    vec4 fogEmissive;         // rgb the global medium's emissive (linear, HDR; Lit colour mode)
    vec4 volumeFrame[MAX_FOG_VOLUMES * 3]; // rows of each volume's world -> volume frame (meters)
    vec4 volumeShape[MAX_FOG_VOLUMES];     // xyz half extents (m), w 0 box / 1 sphere
    vec4 volumeMedium[MAX_FOG_VOLUMES];    // x extinction, y blend distance, z 0 linear / 1 exponential falloff, w noise intensity
    vec4 volumeAlbedo[MAX_FOG_VOLUMES];    // rgb albedo (linear), w 1 / noise scale
    vec4 volumeScroll[MAX_FOG_VOLUMES];    // xyz noise offset (m)
    vec4 lightPosition[MAX_FOG_LIGHTS];    // xyz world position, w 1 point / 2 spot
    vec4 lightColor[MAX_FOG_LIGHTS];       // rgb colour * intensity * volumetric intensity (linear), w range (0 unlimited)
    vec4 lightSpot[MAX_FOG_LIGHTS];        // xyz spot direction, w unused
    vec4 lightCone[MAX_FOG_LIGHTS];        // x inner cone cos, y outer cone cos, z shadow slot (spot: projective, point: first face; -1 none)
} volFog;

#include "includes/depth_util.glsl"
#include "includes/atmosphere.glsl"
#include "includes/height_fog.glsl"
// S10's shadow interface: u_fs_shadow_data and the projective atlas (with SHADOW_DATA_POINT the
// point block and atlas), the single-tap lookups of a ray
#define SHADOW_RAY_TAPS
#include "includes/shadow_sampling.glsl"

// the density factor of volume v at a world point (0 outside, 1 past its blend distance)
float volumeDensity(int v, vec3 p){
    vec3 q = vec3(dot(volFog.volumeFrame[v * 3 + 0], vec4(p, 1.0)),
                  dot(volFog.volumeFrame[v * 3 + 1], vec4(p, 1.0)),
                  dot(volFog.volumeFrame[v * 3 + 2], vec4(p, 1.0)));
    vec3 e = volFog.volumeShape[v].xyz;
    float inside;
    if (volFog.volumeShape[v].w > 0.5){
        inside = (1.0 - length(q / max(e, vec3(1e-4)))) * min(e.x, min(e.y, e.z));
    }else{
        vec3 d = e - abs(q);
        inside = min(d.x, min(d.y, d.z));
    }
    if (inside <= 0.0)
        return 0.0;
    float t = min(inside / max(volFog.volumeMedium[v].y, 1e-4), 1.0);
    if (volFog.volumeMedium[v].z > 0.5)
        return (1.0 - exp(-4.0 * t)) / (1.0 - exp(-4.0));
    return t;
}

float noiseTap(vec3 p){
    return textureLod(sampler3D(u_fogNoise, u_fogNoise_smp), p, 0.0).r;
}

// KHR_lights_punctual's range window (includes/punctual.glsl)
float lightRangeAttenuation(float range, float distance){
    float inverseSquare = 1.0 / max(distance * distance, 1e-4);
    if (range <= 0.0)
        return inverseSquare;
    float r = distance / range;
    return clamp(1.0 - r * r * r * r, 0.0, 1.0) * inverseSquare;
}

vec3 localLights(vec3 p, vec3 dir){
    vec3 radiance = vec3(0.0);
    int lights = int(volFog.noise.w);
    for (int i = 0; i < MAX_FOG_LIGHTS; i++){
        if (i >= lights)
            break;
        vec3 toLight = volFog.lightPosition[i].xyz - p;
        float distance = length(toLight);
        vec3 l = toLight / max(distance, 1e-4);
        float attenuation = lightRangeAttenuation(volFog.lightColor[i].w, distance);
        bool spot = volFog.lightPosition[i].w > 1.5;
        if (spot){
            float cosAngle = dot(normalize(volFog.lightSpot[i].xyz), -l);
            attenuation *= smoothstep(volFog.lightCone[i].y, volFog.lightCone[i].x, cosAngle);
        }
        // one shadow tap where the light reaches
        float shadowSlot = volFog.lightCone[i].z;
        if (shadowSlot > -0.5 && attenuation > 0.0){
            int slot = int(shadowSlot + 0.5);
            #ifdef SHADOW_DATA_POINT
                if (spot)
                    attenuation *= sampleSpotShadow1(slot, volFog.lightPosition[i].xyz, p);
                else
                    attenuation *= samplePointShadow1(slot, volFog.lightPosition[i].xyz, p);
            #else
                if (spot)
                    attenuation *= sampleSpotShadow1(slot, volFog.lightPosition[i].xyz, p);
            #endif
        }
        radiance += volFog.lightColor[i].rgb * (attenuation * heightFogPhase(heightFog.sunDir_anisotropy.w, dot(dir, l)));
    }
    return radiance;
}

void main(){
    vec2 uv = atmosphereFragmentUV(volFog.target.xy);
    vec3 origin;
    vec3 dir;
    atmosphereCameraRay(heightFog.invViewProjection, uv, origin, dir);

    // the surface behind this texel (its farthest, atmosphere_depth.frag): the sky is past any distance
    float linear = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), gl_FragCoord.xy * volFog.target.xy, 0.0));
    float surface = 1e9;
    if (linear < 0.999999){
        float viewDistance = linear * volFog.depthInfo.x;
        surface = (volFog.camera.w > 0.5) ? viewDistance - volFog.depthInfo.y : viewDistance / max(dot(dir, volFog.camera.xyz), 1e-4);
    }
    float marchLength = min(surface, volFog.target.w);

    int steps = int(volFog.target.z);
    float jitter = atmosphereBlueNoise();
    float noiseIntensity = volFog.noise.x;
    int volumes = int(volFog.noise.z);
    int sunTaps = int(volFog.depthInfo.w + 0.5);
    // the two sun taps' offset: 1.5 shadow texels either side, turned by a second static blue
    // noise value (the tile read half a tile over, so it is not the step jitter)
    vec2 sunTapOffset = vec2(0.0);
    if (sunTaps > 1){
        float angle = textureLod(sampler2D(u_blueNoise, u_blueNoise_smp), (gl_FragCoord.xy + vec2(32.0, 32.0)) / 64.0, 0.0).r * 6.2831853;
        sunTapOffset = 1.5 * vec2(cos(angle), sin(angle));
    }
    bool lit = volFog.fogAlbedo.w > 0.5;

    vec3 fogColor = heightFogColor(dir);
    vec3 sunLight = heightFogSunInscatter(dir);
    vec3 scattering = vec3(0.0);
    float transmittance = 1.0;
    float previous = 0.0;
    // steps + 1 parts: [0, u0^2], [u0^2, u1^2], ... [u(n-1)^2, 1] of the march, u_k = (k + jitter) / n
    for (int k = 0; k <= MAX_FOG_STEPS; k++){
        if (k > steps || marchLength <= 0.0)
            break;
        float u = (k == steps) ? 1.0 : (float(k) + jitter) / float(steps);
        float next = marchLength * u * u;
        float t0 = previous;
        float t1 = next;
        previous = next;
        if (t1 <= t0)
            continue;
        float tm = 0.5 * (t0 + t1);
        vec3 p = origin + dir * tm;

        float fogDepth = heightFogOpticalDepth(origin, dir, t0, t1);
        if (noiseIntensity > 0.0){
            // fades out over the last fifth of the volumetric distance: the analytic fog beyond has none
            float fade = 1.0 - smoothstep(0.8, 1.0, tm / max(volFog.target.w, 1e-4));
            float n = noiseTap((p + volFog.noiseOffset.xyz) * volFog.noise.y);
            fogDepth *= mix(1.0, 2.0 * n, noiseIntensity * fade);
        }
        float volumeDepth = 0.0;
        vec3 volumeScatter = vec3(0.0);
        for (int v = 0; v < MAX_FOG_VOLUMES; v++){
            if (v >= volumes)
                break;
            float density = volumeDensity(v, p);
            if (density <= 0.0)
                continue;
            float volumeNoise = volFog.volumeMedium[v].w;
            if (volumeNoise > 0.0)
                density *= mix(1.0, 2.0 * noiseTap((p + volFog.volumeScroll[v].xyz) * volFog.volumeAlbedo[v].w), volumeNoise);
            float depth = volFog.volumeMedium[v].x * density * (t1 - t0);
            volumeDepth += depth;
            volumeScatter += volFog.volumeAlbedo[v].rgb * depth;
        }
        float tau = fogDepth + volumeDepth;
        if (tau <= 0.0)
            continue;

        float visibility = 1.0;
        if (sunTaps > 1)
            visibility = sampleDirectionalShadow2(p, sunTapOffset);
        else if (sunTaps > 0)
            visibility = sampleDirectionalShadow1(p);
        vec3 light = fogColor + sunLight * visibility;
        if (volFog.noise.w > 0.0)
            light += localLights(p, dir);
        // the share of the extinction that scatters, per channel (the fog's albedo is 1 outside
        // the Lit colour mode)
        vec3 fogScatter = lit ? volFog.fogAlbedo.rgb * fogDepth : vec3(fogDepth);
        vec3 albedo = (fogScatter + volumeScatter) / tau;
        float extinction = exp(-tau);
        scattering += transmittance * light * albedo * (1.0 - extinction);
        if (lit)
            scattering += transmittance * volFog.fogEmissive.rgb * (fogDepth / tau) * (1.0 - extinction);
        transmittance *= extinction;
        if (transmittance < 1e-3)
            break;
    }

    // the fog's max opacity, as the analytic fog caps it
    float minTransmittance = 1.0 - heightFog.extinction_base_H_maxOpacity.w;
    if (transmittance < minTransmittance){
        scattering *= (1.0 - minTransmittance) / max(1.0 - transmittance, 1e-6);
        transmittance = minTransmittance;
    }

    frag_color = atmospherePack(scattering, transmittance, volFog.depthInfo.z);
}
