// The main directional light's shadow cascades read from the fragment stage, for passes that
// have no mesh varyings (the volumetric fog march: light shafts). The engine fills the block
// from the camera's shadow uniforms once per pass (sun_shadow_t in AtmosphereRenderer.h) and
// binds the projective shadow atlas with its comparison sampler. One explicit-LOD tap per
// call, so a loop around it stays a loop on every backend (HLSL keeps [loop] with SampleLevel).
//
// Stands in for S10's u_fs_shadow_data / sampleDirectionalShadow1 until that lands, with the
// same function signature.

#ifndef SUN_SHADOW_GLSL
#define SUN_SHADOW_GLSL

#ifndef SUN_SHADOW_CASCADES
#define SUN_SHADOW_CASCADES 4
#endif

uniform u_fs_sunShadow {
    mat4 lightViewProjection[SUN_SHADOW_CASCADES]; // world -> the cascade's clip space
    vec4 atlasRect[SUN_SHADOW_CASCADES];           // xy origin, zw scale of the cascade's slot in the atlas
    vec4 split[SUN_SHADOW_CASCADES];               // x near, y far view depth the cascade covers
    vec4 cameraPosition_cascades;                  // xyz camera world position, w cascades (0 = no shadow)
    vec4 cameraForward_bias;                       // xyz camera world forward, w depth bias
} sunShadow;

uniform texture2D u_shadowAtlas;
uniform samplerShadow u_shadowAtlas_smp;

// the sun's visibility at a world point: 1 lit, 0 in shadow (1 past the last cascade or
// without shadows)
float sampleDirectionalShadow1(vec3 worldPos){
    int cascades = int(sunShadow.cameraPosition_cascades.w);
    float viewDepth = dot(sunShadow.cameraForward_bias.xyz, worldPos - sunShadow.cameraPosition_cascades.xyz);
    for (int c = 0; c < SUN_SHADOW_CASCADES; c++){
        if (c >= cascades)
            break;
        if (viewDepth >= sunShadow.split[c].x && viewDepth <= sunShadow.split[c].y){
            vec4 clip = sunShadow.lightViewProjection[c] * vec4(worldPos, 1.0);
            vec3 coords = (clip.xyz / clip.w) * 0.5 + 0.5;
            if (coords.z >= 1.0)
                return 1.0;
            vec4 rect = sunShadow.atlasRect[c];
            vec2 uv = rect.xy + clamp(coords.xy, vec2(0.0), vec2(1.0)) * rect.zw;
            #ifndef IS_GLSL
                // slots are placed bottom-left up; only GL stores and samples targets bottom-up
                // (includes/shadows.glsl getShadowAtlasUV)
                uv.y = 1.0 - uv.y;
            #endif
            return textureLod(sampler2DShadow(u_shadowAtlas, u_shadowAtlas_smp), vec3(uv, coords.z - sunShadow.cameraForward_bias.w), 0.0);
        }
    }
    return 1.0;
}

#endif
