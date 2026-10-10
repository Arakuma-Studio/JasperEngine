// Unified lighting: the lights reaching the object being drawn, picked per object by the
// engine under one cap (SceneSettings::maxLightsPerObject), in one data layout for both
// stages. Define LIGHTS_VS (in a .vert) or LIGHTS_FS (in a .frag) before including, only in
// the stages that read the lights: GL drivers strip an unused block, which breaks the bind
// slots. The block layout and the functions up to lightShadow are a contract with the
// library shaders; functions may be added, none changed.
//
// Pixel lights are indices [0, pixelLightCount()), the vertex lights follow them, up to
// pixelLightCount() + vertexLightCount(). Loop with the constant LIGHT_CAPACITY bound (HLSL
// unrolls a loop that samples spot masks or shadow maps):
//
//     for (int i = 0; i < LIGHT_CAPACITY; i++){
//         if (i >= pixelLightCount()) break;
//         LightSample light = getLight(i, v_position);
//         diffuse += light.radiance * max(dot(n, light.direction), 0.0);
//     }
//
// Units: radiance is colour * intensity * attenuation, with no 1/PI; ambientLight (scene
// ambient plus the lights over the cap) and vertexDiffuse use the same units. A shader
// dividing its direct light by PI, as the built-in Lit does, scales the light part of the
// ambient the same way with ambientLightScaled.
//
// Fragment stage: with USE_PUNCTUAL, spot lights with a projected mask apply it
// (u_spotMaskAtlas, declared here); with USE_SHADOWS the shadow maps are declared here and
// lightShadow reads the v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS] varyings the vertex stage
// writes (includes/surface_vs.glsl) and the frame block (included here). Vertex lights are
// diffuse only: no specular, no shadows and no mask (the cone still applies).

#ifndef LIGHT_CAPACITY
    // the variant's capacity tier: 4, 8 or 16 lights (mesh property bits 'Lc4' / 'L16')
    #define LIGHT_CAPACITY 8
#endif

#if defined(LIGHTS_VS) && !defined(LIGHTS_VS_DECLARED)
#define LIGHTS_VS_DECLARED
uniform u_vs_objectLights {
    vec4 counts;                          // x pixel lights, y vertex lights (as floats); zw engine-internal
    vec4 shR;                             // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
    vec4 shG;
    vec4 shB;
    vec4 position_type[LIGHT_CAPACITY];   // xyz world position, w type (0 directional, 1 point, 2 spot)
    vec4 direction_range[LIGHT_CAPACITY]; // xyz world direction the light points, w range (<= 0 unlimited)
    vec4 color_intensity[LIGHT_CAPACITY]; // rgb linear colour, w intensity
    vec4 cone_shadow[LIGHT_CAPACITY];     // x inner cone cos, y outer cone cos, z shadow map index (-1 none), w cascades
    vec4 spot_mask[LIGHT_CAPACITY];       // xyz spot projection up, w mask aspect (0 = no mask)
} objectLights;
#endif

#if defined(LIGHTS_FS) && !defined(LIGHTS_FS_DECLARED)
#define LIGHTS_FS_DECLARED
uniform u_fs_objectLights {
    vec4 counts;                          // x pixel lights, y vertex lights (as floats); zw engine-internal
    vec4 shR;                             // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
    vec4 shG;
    vec4 shB;
    vec4 position_type[LIGHT_CAPACITY];   // xyz world position, w type (0 directional, 1 point, 2 spot)
    vec4 direction_range[LIGHT_CAPACITY]; // xyz world direction the light points, w range (<= 0 unlimited)
    vec4 color_intensity[LIGHT_CAPACITY]; // rgb linear colour, w intensity
    vec4 cone_shadow[LIGHT_CAPACITY];     // x inner cone cos, y outer cone cos, z shadow map index (-1 none), w cascades
    vec4 spot_mask[LIGHT_CAPACITY];       // xyz spot projection up, w mask aspect (0 = no mask)
} objectLights;
#endif

// counts.zw (engine-internal): the spot mask atlas tile of each pixel light, 3 bits per
// light, lights 0-7 in z and 8-15 in w

#if (defined(LIGHTS_VS_DECLARED) || defined(LIGHTS_FS_DECLARED)) && !defined(LIGHTS_DECLARED)
#define LIGHTS_DECLARED

#if defined(LIGHTS_FS_DECLARED) && defined(USE_PUNCTUAL)
    #define LIGHTS_SPOT_MASKS
    uniform texture2D u_spotMaskAtlas;
    uniform sampler u_spotMaskAtlas_smp;
#endif

#if defined(LIGHTS_FS_DECLARED) && defined(USE_SHADOWS)
    #ifndef FRAME_FS
        #define FRAME_FS
    #endif
    #include "includes/frame.glsl"

    uniform u_fs_shadows {
        vec4 bias_texSize_nearFar[MAX_SHADOW_ATLAS_SLOTS];
        vec4 atlasRect[MAX_SHADOW_ATLAS_SLOTS];
    } uShadows;

    uniform u_fs_point_shadows {
        vec4 bias_texSize_nearFar[MAX_POINT_SHADOW_ATLAS_SLOTS];
        vec4 atlasRect[MAX_POINT_SHADOW_ATLAS_SLOTS];
    } uPointShadows;

    in vec4 v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS];

    uniform texture2D u_shadowAtlas;
    uniform samplerShadow u_shadowAtlas_smp;
    uniform texture2D u_shadowPointAtlas;
    uniform sampler u_shadowPointAtlas_smp;
#endif

#include "includes/punctual.glsl"
#if defined(LIGHTS_FS_DECLARED) && defined(USE_SHADOWS)
    #include "includes/depth_util.glsl"
    #include "includes/shadows.glsl"
#endif

struct LightSample {
    vec3 direction;    // normalised, from the surface towards the light
    vec3 radiance;     // colour * intensity * attenuation
    float attenuation; // range and cone (or spot mask) attenuation
    int index;
};

int pixelLightCount(){
    return int(objectLights.counts.x);
}

int vertexLightCount(){
    return int(objectLights.counts.y);
}

// light index of the object's list in the form the punctual.glsl functions take
Light getLightData(int index){
    vec4 positionType = objectLights.position_type[index];
    vec4 directionRange = objectLights.direction_range[index];
    vec4 colorIntensity = objectLights.color_intensity[index];
    vec4 coneShadow = objectLights.cone_shadow[index];
    vec4 spotMask = objectLights.spot_mask[index];
    return Light(
        int(positionType.w),
        directionRange.xyz,
        colorIntensity.xyz,
        positionType.xyz,
        directionRange.w,
        colorIntensity.w,
        coneShadow.x,
        coneShadow.y,
        spotMask.xyz,
        spotMask.w,
        (coneShadow.z < 0.0) ? false : true,
        int(coneShadow.z),
        int(coneShadow.w)
    );
}

// from worldPosition towards the light, not normalised (a directional light's reversed direction)
vec3 getLightVector(Light light, vec3 worldPosition){
    if (light.type != LightType_Directional){
        return light.position - worldPosition;
    }
    return -light.direction;
}

LightSample getLight(int index, vec3 worldPosition){
    Light light = getLightData(index);
    vec3 pointToLight = getLightVector(light, worldPosition);

    LightSample lightSample;
    lightSample.direction = normalize(pointToLight);
    #ifdef LIGHTS_SPOT_MASKS
        lightSample.attenuation = getLightAttenuation(light, pointToLight, index);
    #else
        lightSample.attenuation = getLightConeAttenuation(light, pointToLight);
    #endif
    lightSample.radiance = lightSample.attenuation * light.intensity * light.color;
    lightSample.index = index;
    return lightSample;
}

// scene ambient plus the lights over the cap, for a world normal; linear, never negative
vec3 ambientLight(vec3 worldNormal){
    return max(vec3(
        dot(objectLights.shR.xyz, worldNormal) + objectLights.shR.w,
        dot(objectLights.shG.xyz, worldNormal) + objectLights.shG.w,
        dot(objectLights.shB.xyz, worldNormal) + objectLights.shB.w), vec3(0.0));
}

// ambientLight with the part the lights over the cap add scaled by lightScale, so it stays
// in step with a shader's own direct light (Lit: 1/PI). sceneAmbient is the scene's ambient
// (frame.ambient.rgb); with no light over the cap this returns it unchanged.
vec3 ambientLightScaled(vec3 worldNormal, vec3 sceneAmbient, float lightScale){
    return sceneAmbient + max(ambientLight(worldNormal) - sceneAmbient, vec3(0.0)) * lightScale;
}

// sum of the Lambert diffuse (radiance * max(n.l, 0)) of the vertex lights
vec3 vertexDiffuse(vec3 worldPosition, vec3 worldNormal){
    vec3 diffuse = vec3(0.0);
    int first = pixelLightCount();
    int count = vertexLightCount();
    for (int k = 0; k < LIGHT_CAPACITY; k++){
        if (k >= count) break;
        Light light = getLightData(first + k);
        vec3 pointToLight = getLightVector(light, worldPosition);
        float NdotL = max(dot(worldNormal, normalize(pointToLight)), 0.0);
        diffuse += getLightConeAttenuation(light, pointToLight) * light.intensity * light.color * NdotL;
    }
    return diffuse;
}

#if defined(LIGHTS_FS_DECLARED) && defined(USE_SHADOWS)
// 1 = lit, 0 = in shadow; a pixel light without a shadow map is always lit
float lightShadow(int index, vec3 worldPosition, vec3 worldNormal){
    Light light = getLightData(index);
    if (!light.shadows){
        return 1.0;
    }

    vec3 pointToLight = getLightVector(light, worldPosition);
    float NdotL = clamp(dot(worldNormal, normalize(pointToLight)), 0.0, 1.0);
    if (light.type == LightType_Spot){
        return 1.0 - shadowCalculationPCF(light.shadowMapIndex, NdotL);
    }else if (light.type == LightType_Directional){
        float viewDepth = dot(frame.cameraDirection.xyz, worldPosition - frame.cameraPosition.xyz);
        return 1.0 - shadowCascadedCalculationPCF(light.shadowMapIndex, light.numShadowCascades, viewDepth, NdotL);
    }
    return 1.0 - shadowCubeCalculationPCF(light.shadowMapIndex, -pointToLight, NdotL);
}
#endif

#endif // LIGHTS_DECLARED
