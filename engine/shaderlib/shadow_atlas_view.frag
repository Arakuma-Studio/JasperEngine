#version 450

// The Atlas shadow debug view (editor): one shadow atlas's depths as grey (near the light dark),
// drawn into a colour target the Scene view shows over the scene.

in vec2 v_texcoord;
out vec4 frag_color;

// @image_sample_type u_shadowSource unfilterable_float
// @sampler_type u_shadowSource_smp nonfiltering
uniform texture2D u_shadowSource;
uniform sampler u_shadowSource_smp;

uniform u_fs_shadowRestore {
    vec4 sourceRect;   // xy offset, zw scale of the region shown, in the source's UV (bottom-left origin)
    vec4 params;       // unused
} restore;

void main(){
    vec2 uv = v_texcoord;
    #ifndef IS_GLSL
        uv.y = 1.0 - uv.y;
    #endif
    vec2 sourceUV = restore.sourceRect.xy + uv * restore.sourceRect.zw;
    #ifndef IS_GLSL
        sourceUV.y = 1.0 - sourceUV.y;
    #endif
    float depth = textureLod(sampler2D(u_shadowSource, u_shadowSource_smp), sourceUV, 0.0).r;
    // far (empty) stays white; spread the rest for contrast
    float grey = (depth >= 1.0) ? 1.0 : pow(clamp(depth, 0.0, 1.0), 4.0) * 0.9;
    frag_color = vec4(vec3(grey), 1.0);
}
