#version 450

// The vertex colour stays out of v_color (the instance colour still applies).
// SPACE_OBJECT projects in object space: the displacement hook (which moves nothing) hands
// over the object-space position and normal, scaled by the object's scale so the tiling
// stays the same size whatever the scale. In an instanced batch the object is the
// instance: its own space and the scale of the instance and batch together. Its world
// position and normal then reach the fragment stage only for pixel lights.
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
    mat3 model = mat3(pbrParams.modelMatrix);
    vec3 position = objectPosition;
    vec3 normal = objectNormal;
    #ifdef HAS_INSTANCING
        // the hook's position and normal are in the batch's space: back to the instance's
        // (exact without shear: I = R S, so I^-1 = S^-2 I^T; a normal, turned by the
        // inverse transpose, comes back by the transpose, up to its length)
        mat3 instance = mat3(i_matrix_col1.xyz, i_matrix_col2.xyz, i_matrix_col3.xyz);
        vec3 instanceScale2 = vec3(dot(instance[0], instance[0]), dot(instance[1], instance[1]), dot(instance[2], instance[2]));
        position = ((position - i_matrix_col4.xyz) * instance) / max(instanceScale2, vec3(1e-12));
        normal = normal * instance;
        model = model * instance;
    #endif
    vec3 scale = vec3(length(model[0]), length(model[1]), length(model[2]));
    v_objectPosition = position * scale;
    // the normal of the scaled surface
    v_objectNormal = normal / max(scale, vec3(1e-6));
    return objectPosition;
}
#endif
