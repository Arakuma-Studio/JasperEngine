// The dissolve test of library:dissolve, shared by its colour (dissolve.frag) and depth
// (dissolve_depth.frag) fragment stages, so shadows have the same holes. Declares the
// fragment parameter block and the noise texture.
//
// WORLD_NOISE: the noise is sampled triplanar from the world position and normal in
// v_noisePosition / v_noiseNormal, which both vertex stages write the same way
// (dissolveVaryings in library/dissolve_vs.glsl); else from the first UV set.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 noiseTiling;
    vec4 edgeColor;
    float cutoff;
    float edgeWidth;
    float worldNoiseScale;
} customParams;

uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;

#ifdef WORLD_NOISE
    in vec3 v_noisePosition;
    in vec3 v_noiseNormal;
#endif

float dissolveNoise(vec2 uv){
    #ifdef WORLD_NOISE
        // three planar projections blended by the normal, so no UV seam shows
        vec3 p = v_noisePosition * customParams.worldNoiseScale;
        vec3 weights = pow(abs(normalize(v_noiseNormal)), vec3(4.0));
        weights /= dot(weights, vec3(1.0));
        return texture(sampler2D(u_noiseTexture, u_noise_smp), p.zy).r * weights.x +
               texture(sampler2D(u_noiseTexture, u_noise_smp), p.xz).r * weights.y +
               texture(sampler2D(u_noiseTexture, u_noise_smp), p.xy).r * weights.z;
    #else
        return texture(sampler2D(u_noiseTexture, u_noise_smp), uv * customParams.noiseTiling.xy + customParams.noiseTiling.zw).r;
    #endif
}

// The noise level below which a fragment is gone, and the edge band above it: cutoff 0
// keeps everything (no edge either), 1 removes everything
float dissolveEdgeWidth(){
    return max(customParams.edgeWidth, 1e-4);
}

float dissolveClipLevel(){
    float width = dissolveEdgeWidth();
    return customParams.cutoff * (1.0 + width + 1e-3) - width;
}

// 1 at the hole's border, fading to 0 across the edge band
float dissolveEdge(float noise){
    return 1.0 - clamp((noise - dissolveClipLevel()) / dissolveEdgeWidth(), 0.0, 1.0);
}
