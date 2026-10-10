#version 450

// The vertex colour stays out of v_color (the instance colour still applies).
// SPACE_OBJECT projects in object space: the displacement hook (which moves nothing) hands
// over the object-space position and normal, scaled by the object's scale so the tiling
// stays the same size whatever the scale. Its world position and normal then reach the
// fragment stage only for pixel lights.
#define SURFACE_NO_VERTEX_COLOR

#ifdef SPACE_OBJECT
    #define SURFACE_DISPLACE
    #ifndef USE_PUNCTUAL
        #undef SURFACE_POSITION
        #undef SURFACE_NORMALS
    #endif
    out vec3 v_objectPosition;
    out vec3 v_objectNormal;
#endif

#include "includes/surface_vs.glsl"

#ifdef SPACE_OBJECT
vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    vec3 scale = vec3(length(pbrParams.modelMatrix[0].xyz), length(pbrParams.modelMatrix[1].xyz), length(pbrParams.modelMatrix[2].xyz));
    v_objectPosition = objectPosition * scale;
    // the normal of the scaled surface
    v_objectNormal = objectNormal / max(scale, vec3(1e-6));
    return objectPosition;
}
#endif
