#version 450

// The vertex colour stays out of v_color (the instance colour still applies). In vertex
// mode the SH ambient comes apart from the vertex lights (v_vertexAmbient), so the lights
// alone are banded, and v_toonPeak carries what toon.frag bands them against. See
// includes/surface_vs.glsl.
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_VERTEX_AMBIENT
// the world position and normal reach the fragment stage only for pixel lights and the rim
#if !defined(USE_PUNCTUAL) && !defined(RIM)
    #undef SURFACE_POSITION
    #undef SURFACE_NORMALS
#endif
#if defined(USE_VERTEX_LIGHTS) && !defined(USE_PUNCTUAL)
    // the hook moves nothing: it runs once per vertex to write v_toonPeak
    #define TOON_VERTEX_PEAK
    #define SURFACE_DISPLACE
#endif

#include "includes/surface_vs.glsl"

#ifdef TOON_VERTEX_PEAK
// rgb: the vertex lights' radiance / PI, as a surface facing each would take it (the
// vertexDiffuse it gets is this times n.l per light); a: how far they reach the vertex,
// their attenuation without the inverse square falloff, 0 to 1
out vec4 v_toonPeak;

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    vec3 worldPosition = (pbrParams.modelMatrix * vec4(objectPosition, 1.0)).xyz;
    vec3 peak = vec3(0.0);
    float reach = 0.0;
    int first = pixelLightCount();
    int count = vertexLightCount();
    for (int k = 0; k < LIGHT_CAPACITY; k++){
        if (k >= count) break;
        Light light = getLightData(first + k);
        vec3 pointToLight = getLightVector(light, worldPosition);
        float attenuation = getLightConeAttenuation(light, pointToLight);
        peak += attenuation * light.intensity * light.color;
        reach += (light.type != LightType_Directional) ? attenuation * dot(pointToLight, pointToLight) : attenuation;
    }
    v_toonPeak = vec4(peak * LIGHT_INV_PI, min(reach, 1.0));
    return objectPosition;
}
#endif
