#version 450

// Atmosphere/Denoise: one 3x3 depth-aware blur of a reduced-resolution (S, T) target (the
// volumetric fog, the Screen clouds), which removes the static jitter's pattern without
// bleeding across depth edges: each neighbour weighs by a binomial kernel times
// exp(-|z - zCentre| / (0.05 zCentre)) on the linear depths of atmosphere_depth.frag.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_atmosphereTexture;
uniform sampler u_atmosphere_smp;
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_atmosphereDenoise {
    vec4 target;   // xy 1 / target size, z 1: RGBA8 (compressed) texels
} denoise;

#include "includes/depth_util.glsl"
#include "includes/atmosphere.glsl"

void main(){
    vec2 texel = denoise.target.xy;
    vec2 centre = gl_FragCoord.xy * texel;
    float zCentre = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), centre, 0.0));
    float tolerance = 0.05 * max(zCentre, 1e-5);
    vec3 scattering = vec3(0.0);
    float transmittance = 0.0;
    float total = 0.0;
    for (int y = -1; y <= 1; y++){
        for (int x = -1; x <= 1; x++){
            vec2 uv = centre + vec2(float(x), float(y)) * texel;
            float z = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), uv, 0.0));
            float w = ((x == 0) ? 2.0 : 1.0) * ((y == 0) ? 2.0 : 1.0) * exp(-abs(z - zCentre) / tolerance);
            vec3 s;
            float t;
            atmosphereUnpack(textureLod(sampler2D(u_atmosphereTexture, u_atmosphere_smp), uv, 0.0), denoise.target.z, s, t);
            scattering += s * w;
            transmittance += t * w;
            total += w;
        }
    }
    frag_color = atmospherePack(scattering / total, transmittance / total, denoise.target.z);
}
