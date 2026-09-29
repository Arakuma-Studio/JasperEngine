#version 450

// Fullscreen upscale of the fixed-resolution scene color into the real
// destination (fixed game resolution feature). The nearest/linear look comes
// from the source framebuffer sampler, set by Scene::setFixedResolutionFilter.
//
// The source is rendered flipped (PIP_RTT) on GL; when the destination is the
// swapchain (not flipped) params.x = 1 un-flips the sample.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_blitParams {
    vec4 params; // x = flip source Y (GL swapchain destination), yzw unused
} blit;

void main(){
    vec2 uv = v_texcoord;
    if (blit.params.x > 0.5) {
        uv.y = 1.0 - uv.y;
    }
    frag_color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv);
}
