#version 450

// @shader postprocess
// @name "Fixture overlay"
// @texture u_overlayTexture white "Overlay (multiplied, tiled)"
// @property split range(0, 1) 0.5 "The original scene left of this"
// @property overlay range(0, 1) 0.4 "Overlay amount"
// @property tiles float 8 "Overlay tiles across"

// Post-processing fixture (PP_CASE=user_pass): a project post-process pass with a texture of
// its own (@texture, set on the pass) that also reads the scene before the chain
// (u_originalSceneTexture): left of the split the original scene, right of it what the previous
// pass made (a pixelate at half resolution), the overlay multiplied over both and a line at the
// split.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_originalSceneTexture;
uniform sampler u_originalScene_smp;
uniform texture2D u_overlayTexture;
uniform sampler u_overlay_smp;

uniform u_fs_postParams {
    vec4 resolution; // engine-written: xy output size, zw 1 / size
    float split;
    float overlay;
    float tiles;
} postParams;

void main(){
    vec3 previous = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    vec3 original = texture(sampler2D(u_originalSceneTexture, u_originalScene_smp), v_texcoord).rgb;
    vec3 color = v_texcoord.x < postParams.split ? original : previous;
    vec2 tiling = vec2(postParams.tiles, postParams.tiles * postParams.resolution.y * postParams.resolution.z);
    vec3 overlay = texture(sampler2D(u_overlayTexture, u_overlay_smp), v_texcoord * tiling).rgb;
    color = mix(color, color * overlay, postParams.overlay);
    float line = 1.0 - smoothstep(0.0, 2.0 * postParams.resolution.z, abs(v_texcoord.x - postParams.split));
    color = mix(color, vec3(1.0, 0.85, 0.2), line);
    frag_color = vec4(color, 1.0);
}
