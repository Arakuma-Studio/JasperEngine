// Helpers for vertex animation in the SURFACE_DISPLACE hooks (includes/surface_vs.glsl,
// includes/surface_depth_vs.glsl). Include after the surface include: the instance inputs
// are its. The model matrix is an argument, since the colour pass has it in pbrParams and
// a depth program in depthParams; pass the stage's own so both move vertices identically.
//
// Positions in the hooks are in the mesh's space after the instance matrix: for an
// instanced batch, the batch's space, with the instance's transform in i_matrix_col1..4.

#ifndef MOTION_GLSL
#define MOTION_GLSL

const float MOTION_TWO_PI = 6.28318530718;

// world position of the object's origin (of the instance's, in an instanced batch)
vec3 motionObjectOrigin(mat4 modelMatrix){
    #ifdef HAS_INSTANCING
        return (modelMatrix * vec4(i_matrix_col4.xyz, 1.0)).xyz;
    #else
        return modelMatrix[3].xyz;
    #endif
}

// where the hook's pivot is, in the hook's space: the instance's origin, or the object's
vec3 motionLocalOrigin(){
    #ifdef HAS_INSTANCING
        return i_matrix_col4.xyz;
    #else
        return vec3(0.0);
    #endif
}

// the object's (instance's) up axis in the hook's space, normalised
vec3 motionLocalUp(){
    #ifdef HAS_INSTANCING
        return normalize(i_matrix_col2.xyz);
    #else
        return vec3(0.0, 1.0, 0.0);
    #endif
}

// a world-space vector in the hook's space: the inverse of the model matrix's rotation and
// scale, exact without shear (M = R S, so M^-1 = S^-2 M^T)
vec3 motionWorldToLocal(mat4 modelMatrix, vec3 worldVector){
    mat3 m = mat3(modelMatrix);
    return (worldVector * m) / vec3(dot(m[0], m[0]), dot(m[1], m[1]), dot(m[2], m[2]));
}

// a phase in cycles that varies smoothly with a world position, spread so that objects a
// unit apart differ by a large, irregular fraction of a cycle (golden-ratio-like steps)
float motionPhase(vec3 worldPosition){
    return dot(worldPosition, vec3(0.6180, 0.3247, 0.4142));
}

// v rotated about a normalised axis by angle radians (Rodrigues)
vec3 motionRotate(vec3 v, vec3 axis, float angle){
    float s = sin(angle);
    float c = cos(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

#endif
