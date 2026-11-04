// Capsule and contact shadows (docs/specs/S57-forge-lighting.md R2, R3): the occlusion of a pixel
// light lightShadow multiplies into its shadow map's term (includes/lights.glsl, LIGHT_OCCLUSION).
// Each light carries which it takes in its spot_shadow.w bits 12 and 13: 1 the capsule shadows
// (the main directional light), 2 the contact shadows (the light whose mask the frame drew).

#ifndef LIGHT_OCCLUSION_GLSL
#define LIGHT_OCCLUSION_GLSL

// the casters drawn at once times their capsules (render/CapsuleShadows.h MAX_CAPSULES)
#define CAPSULE_SHADOWS_MAX 32

// Engine-internal layout (RenderSystem's light occlusion block)
uniform u_fs_lightOcclusion {
    vec4 contactViewport;    // xy the colour pass's origin in gl_FragCoord pixels, zw 1 / its size
    vec4 contactParams;      // x 1: the mask's rows run the other way (its depth orientation)
    #ifdef USE_CAPSULE_SHADOWS
        vec4 capsuleParams;  // x capsules, y intensity, z softness (penumbra widening a metre)
        vec4 capsuleA[CAPSULE_SHADOWS_MAX];  // xyz one end of the segment, w radius
        vec4 capsuleB[CAPSULE_SHADOWS_MAX];  // xyz the other end
    #endif
} lightOcclusionParams;

#ifdef USE_CONTACT_SHADOWS
    uniform texture2D u_contactShadowTexture;
    uniform sampler u_contactShadow_smp;

    // this pixel's contact shadow (1 lit), from the pass's mask
    float contactShadowMask(){
        vec2 uv = (gl_FragCoord.xy - lightOcclusionParams.contactViewport.xy) * lightOcclusionParams.contactViewport.zw;
        if (lightOcclusionParams.contactParams.x > 0.5){
            uv.y = 1.0 - uv.y;
        }
        // one level: an explicit LOD keeps the lookup legal in the lights' loop
        return textureLod(sampler2D(u_contactShadowTexture, u_contactShadow_smp), uv, 0.0).r;
    }
#endif

#ifdef USE_CAPSULE_SHADOWS
    #include "includes/capsule_shadows.glsl"
#endif

// which occlusions a light takes (bits: 1 capsule, 2 contact)
int lightOcclusionFlags(int index){
    #ifdef LIGHTS_TILED
        return (int(tiledLightsTexel(tiledLightRecord(index) + 3).w) >> 12) & 3;
    #else
        return (int(objectLights.spot_shadow[index].w) >> 12) & 3;
    #endif
}

// 1 = lit, 0 = occluded: the light's capsule and contact shadows at a world position
float lightOcclusion(int index, vec3 worldPosition, vec3 worldNormal){
    int flags = lightOcclusionFlags(index);
    float occlusion = 1.0;
    #ifdef USE_CAPSULE_SHADOWS
        if ((flags & 1) != 0){
            Light light = getLightData(index);
            occlusion *= capsuleShadow(worldPosition, normalize(getLightVector(light, worldPosition)));
        }
    #endif
    #ifdef USE_CONTACT_SHADOWS
        if ((flags & 2) != 0){
            occlusion *= contactShadowMask();
        }
    #endif
    return occlusion;
}

#endif // LIGHT_OCCLUSION_GLSL
