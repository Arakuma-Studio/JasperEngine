#version 450

// Fullscreen copy of a colour target: the upscale of the fixed-resolution scene color
// into the real destination (fixed game resolution feature; the nearest/linear look comes
// from the source framebuffer sampler, set by Scene::setFixedResolutionFilter), the
// scene-colour redirect onto the swapchain, and the scene-colour copy shaders requiring
// scenecolor sample (shader library phase 3).
//
// The source is rendered flipped (PIP_RTT) on GL; when the destination is the swapchain
// (not flipped) params (1, 0, -1, 1) un-flips the sample. A scene-colour copy reads the part
// of its source the colour pass's viewport covered.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_blitParams {
    vec4 params; // source UV = v_texcoord * xz + yw
} blit;

void main(){
    vec2 uv = v_texcoord * blit.params.xz + blit.params.yw;
    frag_color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv);
}
