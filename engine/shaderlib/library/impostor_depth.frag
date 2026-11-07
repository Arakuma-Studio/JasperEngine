#version 450

// Depth program of library:impostor: drops what the colour pass cuts out

#define SURFACE_DEPTH_DISCARD

uniform u_fs_customParams {
    vec4 baseColor;
    float alphaCutoff;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_impostorUv0;
in vec2 v_impostorUv1;
in float v_impostorBlend;
in vec3 v_impostorRight;
in vec3 v_impostorForward;

#include "includes/surface_depth_fs.glsl"

bool surfaceDepthDiscard(vec2 uv, vec4 color){
    float alpha = texture(sampler2D(u_baseTexture, u_base_smp), v_impostorUv0).a;
    if (v_impostorBlend > 0.0) {
        alpha = mix(alpha, texture(sampler2D(u_baseTexture, u_base_smp), v_impostorUv1).a, v_impostorBlend);
    }
    return alpha * customParams.baseColor.a < customParams.alphaCutoff;
}
