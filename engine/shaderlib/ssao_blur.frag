#version 450

// 4x4 box blur of the raw SSAO buffer, matching the 4x4 rotation-noise tile.
// Removes the high-frequency noise introduced by the random kernel rotation.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_ssaoTexture;
uniform sampler u_ssao_smp;

uniform u_fs_ssaoBlurParams {
    vec4 texelSize; // xy = 1.0 / textureSize
} blurParams;

void main(){
    float result = 0.0;
    for (int x = -2; x < 2; ++x){
        for (int y = -2; y < 2; ++y){
            vec2 offset = vec2(float(x), float(y)) * blurParams.texelSize.xy;
            result += texture(sampler2D(u_ssaoTexture, u_ssao_smp), v_texcoord + offset).r;
        }
    }
    frag_color = vec4(vec3(result / 16.0), 1.0);
}
