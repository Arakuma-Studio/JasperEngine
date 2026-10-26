#version 450

// Atmosphere/DepthDownsample: the scene's packed depth (the depth pre-pass, RGBA8
// encodeDepth of the window depth) to a reduced-resolution target of packed linear depth (view
// distance / far), each texel the farthest of the factor x factor source texels it covers, so
// the volumetric march at that texel reaches the farthest surface behind it (the composite's
// bilateral upsample picks per pixel). Both targets keep the same row order (atmosphere.glsl),
// so a texel's source block is found in storage coordinates.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_atmosphereDepth {
    vec4 source;              // xy 1 / source size, zw source size
    vec4 factor_invFar;       // x texels per axis (2 or 4), y 1 / far clip distance
    vec4 inverseProjectionZ;  // view distance = dot(z, ndc) / dot(w, ndc), ndc = (camera uv * 2 - 1, depth * 2 - 1, 1)
    vec4 inverseProjectionW;
} depthParams;

#include "includes/depth_util.glsl"
#include "includes/atmosphere.glsl"

void main(){
    int factor = int(depthParams.factor_invFar.x);
    vec2 base = floor(gl_FragCoord.xy) * float(factor);
    float farthest = 0.0;
    for (int y = 0; y < 4; y++){
        if (y >= factor)
            break;
        for (int x = 0; x < 4; x++){
            if (x >= factor)
                break;
            vec2 texel = min(base + vec2(float(x), float(y)), depthParams.source.zw - 1.0) + 0.5;
            vec2 uv = texel * depthParams.source.xy;
            float depth01 = min(decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), uv, 0.0)), 1.0);
            float linear = 1.0;
            if (depth01 < 1.0){
                vec4 ndc = vec4(atmosphereTargetUV(uv) * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
                linear = dot(depthParams.inverseProjectionZ, ndc) / dot(depthParams.inverseProjectionW, ndc) * depthParams.factor_invFar.y;
            }
            farthest = max(farthest, linear);
        }
    }
    // 1.0 does not pack (depth_util.glsl): the far plane and the sky are its last step below
    frag_color = encodeDepth(clamp(farthest, 0.0, 0.9999999));
}
