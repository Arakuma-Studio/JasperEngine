#version 450

// library:projected_decal: the shared vertex path for the box, plus the box's world ->
// decal-space transform for the fragment stage: the inverse of its model matrix (times the
// instance matrix of an instanced box), as the three rows of an affine 3x4 matrix, the same
// at every vertex (flat). Computed per vertex from the matrices the draw already has (a
// box has 8 to 24 vertices), so the decal needs no script to feed it its own transform.
// The displacement hook only reads the matrices; the position is unchanged. With
// ANGLE_FADE it also passes the cosines of the two fade angles (once per vertex, not per
// pixel).

// the instance colour only (a tint per instanced decal); the box's vertex colour is not read
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"

flat out vec4 v_decalRow0;
flat out vec4 v_decalRow1;
flat out vec4 v_decalRow2;

#ifdef ANGLE_FADE
    uniform u_vs_customParams {
        float angleFadeStart; // degrees
        float angleFadeEnd;
    } customParams;

    flat out vec2 v_angleFade;
#endif

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    mat4 boxMatrix = pbrParams.modelMatrix;
    #ifdef HAS_INSTANCING
        boxMatrix = boxMatrix * mat4(i_matrix_col1, i_matrix_col2, i_matrix_col3, i_matrix_col4);
    #endif
    // affine inverse: [A | t]^-1 = [A^-1 | -A^-1 t]
    mat3 inverseAxes = inverse(mat3(boxMatrix));
    vec3 translation = -(inverseAxes * boxMatrix[3].xyz);
    v_decalRow0 = vec4(inverseAxes[0][0], inverseAxes[1][0], inverseAxes[2][0], translation.x);
    v_decalRow1 = vec4(inverseAxes[0][1], inverseAxes[1][1], inverseAxes[2][1], translation.y);
    v_decalRow2 = vec4(inverseAxes[0][2], inverseAxes[1][2], inverseAxes[2][2], translation.z);
    #ifdef ANGLE_FADE
        v_angleFade = cos(radians(vec2(customParams.angleFadeStart, customParams.angleFadeEnd)));
    #endif
    return objectPosition;
}
