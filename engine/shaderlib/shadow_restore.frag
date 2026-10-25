#version 450

// The static shadow cache's copy into the shadow atlas, and the clear of one atlas slot
// (RenderSystem::renderShadowMaps, docs/rendering/shadows.md): drawn as a fullscreen triangle
// into a depth-only pass, the viewport on the destination slot and the scissor on the rect to
// write, depth write on and a depth test that lets every new value through (NOT_EQUAL: Vulkan
// writes no depth with the test off). No colour output.

in vec2 v_texcoord;

// the static cache atlas, its depths themselves
// @image_sample_type u_shadowSource unfilterable_float
// @sampler_type u_shadowSource_smp nonfiltering
uniform texture2D u_shadowSource;
uniform sampler u_shadowSource_smp;

uniform u_fs_shadowRestore {
    vec4 sourceRect;   // xy offset, zw scale of the source slot in the source's UV (bottom-left origin)
    vec4 params;       // x 1: clear to the far plane (no copy)
} restore;

void main(){
    if (restore.params.x > 0.5){
        gl_FragDepth = 1.0;
        return;
    }
    // v_texcoord addresses the texel written in storage order; the rects are bottom-left
    vec2 uv = v_texcoord;
    #ifndef IS_GLSL
        uv.y = 1.0 - uv.y;
    #endif
    vec2 sourceUV = restore.sourceRect.xy + uv * restore.sourceRect.zw;
    #ifndef IS_GLSL
        sourceUV.y = 1.0 - sourceUV.y;
    #endif
    gl_FragDepth = textureLod(sampler2D(u_shadowSource, u_shadowSource_smp), sourceUV, 0.0).r;
}
