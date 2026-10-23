#version 450

// Atmosphere/VolumetricComposite: drawn in the colour pass after the sky composite, a
// fullscreen triangle without depth test over every pixel: the volumetric fog (S_v, T_v) and
// the Screen clouds (S_c, T_c), reduced-resolution both, upsampled to this pixel and laid over
// the scene, the clouds behind the fog:
//   colour' = colour T_c T_v + S_c T_v + S_v
// The upsample is joint bilateral: the 4 nearest reduced texels weigh bilinearly times
// exp(-|z_low - z_full| / (0.05 z_full)) on linear depths, so the fog of a near silhouette
// does not leak onto the far scene behind it; where every weight vanishes, the texel of the
// nearest depth is taken.

in vec2 v_texcoord;
out vec4 frag_color;

// this pass's full-resolution packed depth (the depth pre-pass)
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
uniform texture2D u_fogTexture;
uniform sampler u_fog_smp;
uniform texture2D u_fogDepth;
uniform sampler u_fogDepth_smp;
uniform texture2D u_cloudsTexture;
uniform sampler u_clouds_smp;
uniform texture2D u_cloudsDepth;
uniform sampler u_cloudsDepth_smp;

uniform u_fs_volumetricComposite {
    vec4 screenToUV;          // camera uv = gl_FragCoord.xy * xy + zw
    vec4 fogSize;             // xy 1 / fog target size, zw its size
    vec4 cloudsSize;          // xy 1 / clouds target size, zw its size
    vec4 params;              // x 1: fog, y 1: clouds, z 1: RGBA8 (compressed) targets, w 1 / far clip
    vec4 inverseProjectionZ;  // view distance = dot(z, ndc) / dot(w, ndc)
    vec4 inverseProjectionW;
} composite;

#define USE_POST_COLOR
#include "includes/output.glsl"
#include "includes/depth_util.glsl"
#include "includes/atmosphere.glsl"

// a texel of a layer (0 the fog, 1 the clouds) and of its linear depth
vec4 layerTexel(int layer, vec2 uv){
    if (layer == 0)
        return textureLod(sampler2D(u_fogTexture, u_fog_smp), uv, 0.0);
    return textureLod(sampler2D(u_cloudsTexture, u_clouds_smp), uv, 0.0);
}

float layerDepth(int layer, vec2 uv){
    if (layer == 0)
        return decodeDepth(textureLod(sampler2D(u_fogDepth, u_fogDepth_smp), uv, 0.0));
    return decodeDepth(textureLod(sampler2D(u_cloudsDepth, u_cloudsDepth_smp), uv, 0.0));
}

// (S, T) of a layer's reduced target at this pixel, by the joint bilateral weights
void upsample(int layer, vec4 size, vec2 cameraUV, float zFull, out vec3 scattering, out float transmittance){
    vec2 position = atmosphereTargetUV(cameraUV) * size.zw - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    float tolerance = 0.05 * max(zFull, 1e-5);
    vec3 sum = vec3(0.0);
    float sumT = 0.0;
    float total = 0.0;
    float nearest = 1e10;
    vec2 nearestUV = (base + 0.5) * size.xy;
    for (int i = 0; i < 4; i++){
        vec2 offset = vec2(float(i & 1), float(i >> 1));
        vec2 uv = clamp(base + offset + 0.5, vec2(0.5), size.zw - 0.5) * size.xy;
        float z = layerDepth(layer, uv);
        float bilinear = (offset.x > 0.5 ? f.x : 1.0 - f.x) * (offset.y > 0.5 ? f.y : 1.0 - f.y);
        float w = bilinear * exp(-abs(z - zFull) / tolerance);
        vec3 s;
        float t;
        atmosphereUnpack(layerTexel(layer, uv), composite.params.z, s, t);
        sum += s * w;
        sumT += t * w;
        total += w;
        float difference = abs(z - zFull);
        if (difference < nearest){
            nearest = difference;
            nearestUV = uv;
        }
    }
    if (total > 1e-4){
        scattering = sum / total;
        transmittance = sumT / total;
    }else{
        atmosphereUnpack(layerTexel(layer, nearestUV), composite.params.z, scattering, transmittance);
    }
}

void main(){
    vec2 cameraUV = gl_FragCoord.xy * composite.screenToUV.xy + composite.screenToUV.zw;
    float depth01 = min(decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), atmosphereTargetUV(cameraUV), 0.0)), 1.0);
    float zFull = 0.9999999;
    if (depth01 < 1.0){
        vec4 ndc = vec4(cameraUV * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
        zFull = min(dot(composite.inverseProjectionZ, ndc) / dot(composite.inverseProjectionW, ndc) * composite.params.w, 0.9999999);
    }

    vec3 fogScattering = vec3(0.0);
    float fogTransmittance = 1.0;
    if (composite.params.x > 0.5)
        upsample(0, composite.fogSize, cameraUV, zFull, fogScattering, fogTransmittance);
    vec3 cloudScattering = vec3(0.0);
    float cloudTransmittance = 1.0;
    if (composite.params.y > 0.5)
        upsample(1, composite.cloudsSize, cameraUV, zFull, cloudScattering, cloudTransmittance);

    vec3 scattering = cloudScattering * fogTransmittance + fogScattering;
    float transmittance = cloudTransmittance * fogTransmittance;
    frag_color = atmosphereLayerColor(scattering, transmittance);
}
