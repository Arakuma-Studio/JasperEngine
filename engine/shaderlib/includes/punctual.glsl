// KHR_lights_punctual attenuation, shared by both stages: includes/lights.glsl evaluates
// pixel lights with it in the fragment stage and vertex lights in the vertex stage. A fork
// declaring the legacy u_fs_lighting block includes it directly, after that block and the
// u_spotMaskAtlas texture.
#ifndef PUNCTUAL_GLSL
#define PUNCTUAL_GLSL

struct Light{
    int type;

    vec3 direction;
    vec3 color;
    vec3 position;

    float range;
    float intensity;

    float innerConeCos;
    float outerConeCos;

    vec3 spotUp;
    float spotMaskAspect;

    bool shadows;
    int shadowMapIndex;
    int numShadowCascades;
};

const int LightType_Directional = 0;
const int LightType_Point = 1;
const int LightType_Spot = 2;

// https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_lights_punctual/README.md#range-property
float getRangeAttenuation(float range, float distance)
{
    if (range <= 0.0)
    {
        // negative range means unlimited
        return 1.0 / pow(distance, 2.0);
    }
    return max(min(1.0 - pow(distance / range, 4.0), 1.0), 0.0) / pow(distance, 2.0);
}

// https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_lights_punctual/README.md#inner-and-outer-cone-angles
float getSpotAttenuation(vec3 pointToLight, vec3 spotDirection, float outerConeCos, float innerConeCos)
{
    float actualCos = dot(normalize(spotDirection), normalize(-pointToLight));
    if (actualCos > outerConeCos)
    {
        if (actualCos < innerConeCos)
        {
            return smoothstep(outerConeCos, innerConeCos, actualCos);
        }
        return 1.0;
    }
    return 0.0;
}

// range and cone attenuation, without the projected spot mask (vertex lights)
float getLightConeAttenuation(Light light, vec3 pointToLight)
{
    float rangeAttenuation = 1.0;
    float spotAttenuation = 1.0;

    if (light.type != LightType_Directional)
    {
        rangeAttenuation = getRangeAttenuation(light.range, length(pointToLight));
    }
    if (light.type == LightType_Spot)
    {
        spotAttenuation = getSpotAttenuation(pointToLight, light.direction, light.outerConeCos, light.innerConeCos);
    }

    return rangeAttenuation * spotAttenuation;
}

// Projected spot masks share one horizontal atlas of SPOT_MASK_TILES tiles. The tile of a
// light comes from the per-object lights block (lights.glsl, spot_shadow.w), or, for a fork declaring the
// legacy u_fs_lighting block, from eyePos.w (3 bits per legacy light index).
#if defined(LIGHTS_SPOT_MASKS) || !defined(LIGHTS_DECLARED)

#ifndef SPOT_MASK_TILES
    #define SPOT_MASK_TILES MAX_LIGHTS
#endif

int getSpotMaskTile(int lightIndex)
{
    #if defined(LIGHTS_SPOT_MASKS) && defined(LIGHTS_TILED)
        return (int(tiledLightsTexel(tiledLightRecord(lightIndex) + 3).w) >> 9) & 7;
    #elif defined(LIGHTS_SPOT_MASKS)
        return (int(objectLights.spot_shadow[lightIndex].w) >> 9) & 7;
    #else
        return (int(lighting.eyePos.w) >> (3 * lightIndex)) & 7;
    #endif
}

float getSpotMaskAttenuation(Light light, vec3 pointToLight, int lightIndex)
{
    vec3 forward = normalize(light.direction);
    vec3 up = light.spotUp - forward * dot(light.spotUp, forward);
    float upLength = length(up);
    if (upLength <= 0.0001)
    {
        return 0.0;
    }
    up /= upLength;
    vec3 right = normalize(cross(forward, up));

    vec3 ray = normalize(-pointToLight);
    float forwardDistance = dot(ray, forward);
    if (forwardDistance <= 0.0)
    {
        return 0.0;
    }

    float outerSin = sqrt(max(1.0 - light.outerConeCos * light.outerConeCos, 0.0));
    float outerTan = outerSin / max(light.outerConeCos, 0.0001);
    vec2 halfExtents = outerTan * vec2(light.spotMaskAspect, 1.0);
    vec2 projected = vec2(dot(ray, right), dot(ray, up)) / forwardDistance;
    vec2 maskPosition = projected / max(halfExtents, vec2(0.0001));

    if (any(greaterThan(abs(maskPosition), vec2(1.0))))
    {
        return 0.0;
    }

    // JasperEngine texture coordinates already match the uploaded image orientation.
    vec2 localUV = maskPosition * 0.5 + 0.5;
    ivec2 atlasDimensionsI = textureSize(
        sampler2D(u_spotMaskAtlas, u_spotMaskAtlas_smp),
        0);
    vec2 atlasDimensions = vec2(atlasDimensionsI);
    vec2 tileDimensions = vec2(atlasDimensions.x / float(SPOT_MASK_TILES), atlasDimensions.y);
    vec2 inset = 0.5 / tileDimensions;
    localUV = clamp(localUV, inset, 1.0 - inset);

    vec2 atlasUV = vec2(
        (float(getSpotMaskTile(lightIndex)) + localUV.x) / float(SPOT_MASK_TILES),
        localUV.y
    );
    // the atlas has one level: an explicit LOD keeps the lookup legal in any loop
    return textureLod(sampler2D(u_spotMaskAtlas, u_spotMaskAtlas_smp), atlasUV, 0.0).r;
}

// range and cone attenuation, the projected mask replacing the cone when the spot has one
float getLightAttenuation(Light light, vec3 pointToLight, int lightIndex)
{
    float rangeAttenuation = 1.0;
    float spotAttenuation = 1.0;

    if (light.type != LightType_Directional)
    {
        rangeAttenuation = getRangeAttenuation(light.range, length(pointToLight));
    }
    if (light.type == LightType_Spot)
    {
        if (light.spotMaskAspect > 0.0)
        {
            spotAttenuation = getSpotMaskAttenuation(light, pointToLight, lightIndex);
        }
        else
        {
            spotAttenuation = getSpotAttenuation(pointToLight, light.direction, light.outerConeCos, light.innerConeCos);
        }
    }

    return rangeAttenuation * spotAttenuation;
}

vec3 getLighIntensity(Light light, vec3 pointToLight, int lightIndex)
{
    return getLightAttenuation(light, pointToLight, lightIndex) * light.intensity * light.color;
}

#elif defined(DIRECTIONAL_LIGHTS_ONLY)

// a directional-only variant ('Dlo') has no spot light to mask: the same functions, unattenuated
float getLightAttenuation(Light light, vec3 pointToLight, int lightIndex)
{
    return getLightConeAttenuation(light, pointToLight);
}

vec3 getLighIntensity(Light light, vec3 pointToLight, int lightIndex)
{
    return getLightAttenuation(light, pointToLight, lightIndex) * light.intensity * light.color;
}

#endif // spot masks

#endif // PUNCTUAL_GLSL
