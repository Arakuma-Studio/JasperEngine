// Unified lighting: the lights reaching the object being drawn, picked per object by the
// engine under one cap (SceneSettings::maxLightsPerObject), in one data layout for both
// stages. Define LIGHTS_VS (in a .vert) or LIGHTS_FS (in a .frag) before including, only in
// the stages that read the lights: GL drivers strip an unused block, which breaks the bind
// slots. The functions (getLight to lightShadow) are a contract with the library shaders;
// functions may be added, none changed. The blocks' layout is engine-internal: never read
// objectLights directly.
//
// Pixel lights are indices [0, pixelLightCount()), the vertex lights follow them, up to
// pixelLightCount() + vertexLightCount(). Loop with the constant LIGHT_CAPACITY bound (HLSL
// unrolls a loop that samples spot masks or shadow maps):
//
//     vec3 light = ambientLight(n);
//     for (int i = 0; i < LIGHT_CAPACITY; i++){
//         if (i >= pixelLightCount()) break;
//         LightSample s = getLight(i, v_position);
//         light += lambert(s, n) * lightShadow(i, v_position, n); // lightShadow: USE_SHADOWS only
//     }
//     color = albedo * light;
//
// Units, the one convention for every shader: ambientLight, vertexDiffuse, lambert,
// blinnPhong and ggxSpecular return light to multiply directly by the surface colour (albedo
// for diffuse, the specular colour for blinnPhong and ggxSpecular). Lambert's 1/PI is inside them, as in the built-in
// Lit's BRDF_lambertian, so one light looks equally bright in Lit and in a library shader,
// per pixel and per vertex. Only LightSample.radiance (colour * intensity * attenuation)
// is the raw light, for a shader with its own BRDF: divide its diffuse by PI to match.
// ambientLight is the scene ambient (frame.ambient) plus the lights over the cap, folded
// by the engine in the same units. The vertex stage has it only in vertex mode (no
// USE_PUNCTUAL); in mixed mode the fragment stage adds it.
//
// In a library shader (includes/surface_vs.glsl):
//   - USE_PUNCTUAL (pixel, mixed): the fragment stage declares LIGHTS_FS, adds
//     ambientLight(n) and the pixel lights; v_position and v_normal are always written.
//   - USE_VERTEX_LIGHTS (vertex, mixed): v_vertexLight holds vertexDiffuse, plus
//     ambientLight when USE_PUNCTUAL is off (vertex mode: the fragment stage has no lights
//     block). Add it as is: light += v_vertexLight.
//   - USE_SHADOWS (pixel lights that receive shadows): call lightShadow for the pixel
//     lights, which reads the v_lightProjPos[] varyings surface_vs.glsl writes (an unread
//     varying fails the cross-compile).
//
// Fragment stage: with USE_PUNCTUAL, spot lights with a projected mask apply it
// (u_spotMaskAtlas, declared here); with USE_SHADOWS the shadow maps are declared here and
// lightShadow reads the v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS] varyings the vertex stage
// writes (includes/surface_vs.glsl) and the frame block (included here; the built-in Lit
// reads its own camera block, includes/lit_frame.glsl, instead). Vertex lights are
// diffuse only: no specular, no shadows and no mask (the cone still applies).

#ifndef LIGHT_CAPACITY
    // the variant's capacity tier: 4, 8 or 16 lights (mesh property bits 'Lc4' / 'L16')
    #define LIGHT_CAPACITY 8
#endif

// The block layout is engine-internal (RenderSystem::buildObjectLights); shaders use the
// functions below. Four vectors per light, packed so the vertex stage needs only the first
// three arrays (a prefix of the fragment stage's block, which the engine uploads alone):
//   position_type    xyz world position; w 0 for a directional light, else 1 + range
//                    (range 0: unlimited), so w < 0.5 tells a directional light
//   direction_inner  xyz world direction the light points; w spot inner cone cos (-2 point)
//   color_outer      rgb linear colour * intensity; w spot outer cone cos (-2 point)
//   spot_shadow      xy spot projection up (octahedral), z mask aspect (0 = no mask),
//                    w (shadow map index + 1) | cascades << 6 | mask atlas tile << 9
// The SH ambient follows the vertex arrays: the vertex stage declares it only in vertex
// mode (no USE_PUNCTUAL), where it adds the ambient itself.

#if defined(LIGHTS_VS) && !defined(LIGHTS_VS_DECLARED)
#define LIGHTS_VS_DECLARED
uniform u_vs_objectLights {
    vec4 counts;                          // x pixel lights, y vertex lights (as floats)
    vec4 position_type[LIGHT_CAPACITY];
    vec4 direction_inner[LIGHT_CAPACITY];
    vec4 color_outer[LIGHT_CAPACITY];
    #ifndef USE_PUNCTUAL
        vec4 shR;                         // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
        vec4 shG;
        vec4 shB;
    #endif
} objectLights;
#endif

#if defined(LIGHTS_FS) && !defined(LIGHTS_FS_DECLARED)
#define LIGHTS_FS_DECLARED
uniform u_fs_objectLights {
    vec4 counts;                          // x pixel lights, y vertex lights (as floats)
    vec4 position_type[LIGHT_CAPACITY];
    vec4 direction_inner[LIGHT_CAPACITY];
    vec4 color_outer[LIGHT_CAPACITY];
    vec4 shR;                             // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
    vec4 shG;
    vec4 shB;
    vec4 spot_shadow[LIGHT_CAPACITY];
} objectLights;
#endif

#if (defined(LIGHTS_VS_DECLARED) || defined(LIGHTS_FS_DECLARED)) && !defined(LIGHTS_DECLARED)
#define LIGHTS_DECLARED

#if defined(LIGHTS_FS_DECLARED) && defined(USE_PUNCTUAL)
    #define LIGHTS_SPOT_MASKS
    uniform texture2D u_spotMaskAtlas;
    uniform sampler u_spotMaskAtlas_smp;
#endif

#if defined(LIGHTS_FS_DECLARED) && defined(USE_SHADOWS)
    // the camera the cascades are picked by: the built-in Lit's own block, else the frame block
    #ifdef LIT_FRAME_DECLARED
        #define LIGHTS_CAMERA_POSITION litFrame.cameraPosition_fogEnd.xyz
        #define LIGHTS_CAMERA_FORWARD litFrame.cameraForward_pcf.xyz
    #else
        #ifndef FRAME_FS
            #define FRAME_FS
        #endif
        #include "includes/frame.glsl"
        #define LIGHTS_CAMERA_POSITION frame.cameraPosition.xyz
        #define LIGHTS_CAMERA_FORWARD frame.cameraDirection.xyz
    #endif

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

// the spot projection up of an octahedral-encoded unit vector (xy in [-1, 1])
vec3 lightsOctahedralDecode(vec2 e){
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-v.z, 0.0);
    v.x += (v.x >= 0.0) ? -t : t;
    v.y += (v.y >= 0.0) ? -t : t;
    return normalize(v);
}

// light index of the object's list in the form the punctual.glsl functions take; colour is
// premultiplied by the intensity, which is 1. The vertex stage leaves out what only pixel
// lights read (spot projection, mask, shadows).
Light getLightData(int index){
    vec4 positionType = objectLights.position_type[index];
    vec4 directionInner = objectLights.direction_inner[index];
    vec4 colorOuter = objectLights.color_outer[index];
    int type = (positionType.w < 0.5) ? LightType_Directional : ((colorOuter.w < -1.5) ? LightType_Point : LightType_Spot);
    vec3 spotUp = vec3(0.0, 1.0, 0.0);
    float maskAspect = 0.0;
    int shadowMapIndex = -1;
    int cascades = 0;
    #ifdef LIGHTS_FS_DECLARED
        vec4 spotShadow = objectLights.spot_shadow[index];
        spotUp = lightsOctahedralDecode(spotShadow.xy);
        maskAspect = spotShadow.z;
        int packedShadow = int(spotShadow.w);
        shadowMapIndex = (packedShadow & 63) - 1;
        cascades = (packedShadow >> 6) & 7;
    #endif
    return Light(
        type,
        directionInner.xyz,
        colorOuter.xyz,
        positionType.xyz,
        positionType.w - 1.0,
        1.0,
        directionInner.w,
        colorOuter.w,
        spotUp,
        maskAspect,
        shadowMapIndex >= 0,
        shadowMapIndex,
        cascades
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

// 1/PI: Lambert's normalisation, inside every helper below
const float LIGHT_INV_PI = 0.31830988618;

#if defined(LIGHTS_FS_DECLARED) || !defined(USE_PUNCTUAL)
// scene ambient plus the lights over the cap, for a world normal; light to multiply by
// albedo, linear, never negative
vec3 ambientLight(vec3 worldNormal){
    return max(vec3(
        dot(objectLights.shR.xyz, worldNormal) + objectLights.shR.w,
        dot(objectLights.shG.xyz, worldNormal) + objectLights.shG.w,
        dot(objectLights.shB.xyz, worldNormal) + objectLights.shB.w), vec3(0.0));
}
#endif

// Lambert diffuse of one light: light to multiply by albedo (radiance * max(n.l, 0) / PI)
vec3 lambert(LightSample lightSample, vec3 worldNormal){
    return lightSample.radiance * (max(dot(worldNormal, lightSample.direction), 0.0) * LIGHT_INV_PI);
}

// Normalised Blinn-Phong specular of one light, light to multiply by the specular colour:
// radiance * n.l * (shininess + 8) / (8 PI) * max(n.h, 0)^shininess. viewDirection is from
// the surface towards the camera, normalised.
vec3 blinnPhong(LightSample lightSample, vec3 worldNormal, vec3 viewDirection, float shininess){
    float NdotL = max(dot(worldNormal, lightSample.direction), 0.0);
    vec3 halfway = normalize(lightSample.direction + viewDirection);
    float NdotH = max(dot(worldNormal, halfway), 0.0);
    return lightSample.radiance * (NdotL * (shininess + 8.0) * (0.125 * LIGHT_INV_PI) * pow(NdotH, shininess));
}

// GGX (Trowbridge-Reitz) specular of one light with Schlick's fresnel and the Smith joint
// visibility, as the built-in Lit's BRDF_specularGGX, light to multiply by the specular
// colour: radiance * n.l * D * Vis * F. roughness is perceptual (0 mirror .. 1), squared
// inside (at least 0.045, so a sun's glint stays finite); f0 the reflectance at normal
// incidence (0.02 water, 0.04 most dielectrics). viewDirection as blinnPhong.
vec3 ggxSpecular(LightSample lightSample, vec3 worldNormal, vec3 viewDirection, float roughness, float f0){
    float NdotL = max(dot(worldNormal, lightSample.direction), 0.0);
    float NdotV = max(dot(worldNormal, viewDirection), 1e-4);
    vec3 halfway = normalize(lightSample.direction + viewDirection);
    float NdotH = max(dot(worldNormal, halfway), 0.0);
    float VdotH = max(dot(viewDirection, halfway), 0.0);
    float alpha = max(roughness, 0.045);
    alpha *= alpha;
    float alpha2 = alpha * alpha;
    float f = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 * LIGHT_INV_PI / (f * f);
    float visibility = 0.5 / max(NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2) +
                                 NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2), 1e-5);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
    return lightSample.radiance * (NdotL * distribution * visibility * fresnel);
}

// sum of the Lambert diffuse of the vertex lights, light to multiply by albedo (as lambert)
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
    return diffuse * LIGHT_INV_PI;
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
        float viewDepth = dot(LIGHTS_CAMERA_FORWARD, worldPosition - LIGHTS_CAMERA_POSITION);
        return 1.0 - shadowCascadedCalculationPCF(light.shadowMapIndex, light.numShadowCascades, viewDepth, NdotL);
    }
    return 1.0 - shadowCubeCalculationPCF(light.shadowMapIndex, -pointToLight, NdotL);
}
#endif

#endif // LIGHTS_DECLARED
