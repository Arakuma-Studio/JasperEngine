#version 450

// The resolve of weighted blended order-independent transparency (docs/specs/S57-forge-lighting.md
// R4, includes/oit.glsl): the weighted average of the transparent layers' colours, with the
// coverage 1 - revealage, over the scene (blended SRC_ALPHA, ONE_MINUS_SRC_ALPHA in the camera's
// colour pass). The accumulation targets match the colour target texel for texel.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_oitAccumTexture;
uniform sampler u_oitAccum_smp;
uniform texture2D u_oitWeightTexture;
uniform sampler u_oitWeight_smp;

void main(){
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec4 accum = texelFetch(sampler2D(u_oitAccumTexture, u_oitAccum_smp), texel, 0);
    float weight = texelFetch(sampler2D(u_oitWeightTexture, u_oitWeight_smp), texel, 0).r;
    float revealage = clamp(accum.a, 0.0, 1.0);
    // no transparent layer here: revealage 1, coverage 0, the scene stays as it is
    vec3 average = accum.rgb / max(weight, 1e-5);
    frag_color = vec4(clamp(average, vec3(0.0), vec3(1.0)), 1.0 - revealage);
}
