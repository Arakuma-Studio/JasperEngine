#version 450

// Scene-inputs fixture (SI_POST=1): a post-process pass that samples the depth, which the
// engine draws with a depth pre-pass when neither SSAO nor SSR runs; it darkens the far
// half of the scene a little, so its depth shows.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

#include "includes/depth_util.glsl"

void main(){
    vec4 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord);
    // depth keeps the logical orientation: flip on GL
    vec2 depthUV = v_texcoord;
    #ifdef IS_GLSL
        depthUV.y = 1.0 - depthUV.y;
    #endif
    float depth = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), depthUV));
    frag_color = vec4(color.rgb * (depth > 0.995 ? 0.7 : 1.0), color.a);
}
