#version 450

// Built-in post-process pass: a passthrough of the scene color. It is the template a
// user fork starts from and the fallback when a fork fails to build.
//
// A fork may declare any subset of the pass inputs; only the ones it uses are bound:
// u_sceneColorTexture (previous pass output), u_depthTexture (camera-space packed
// depth), u_gbufferTexture (view-space normal + roughness/metallic, SSR only) and
// u_ssaoTexture. Depth and the G-buffer keep the logical orientation of the depth
// pre-pass, so flip v_texcoord.y on GL when sampling them (see ssr.frag).
//
// Declaring a u_fs_postParams block makes every member an editable row in the scene
// post-process list. Do not mix int and float members: GL flattens the block into one
// upload typed after the first member. Two names are written by the engine instead of
// being edited: resolution (xy = size, zw = 1 / size) and time (seconds).
//
//   uniform u_fs_postParams {
//       vec4 resolution;
//       float amount;
//   } postParams;

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

void main(){
    frag_color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord);
}
