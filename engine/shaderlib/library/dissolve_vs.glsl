// The vertex side of library:dissolve's world noise (WORLD_NOISE), shared by its colour
// (dissolve.vert) and depth (dissolve_depth.vert) programs: both write the world position
// and normal the noise is sampled from with the same expressions, so the holes match.
// Include after the surface include.

#ifdef WORLD_NOISE
    out vec3 v_noisePosition;
    out vec3 v_noiseNormal;

    // called from the stage's surfaceDisplace hook, which moves nothing
    void dissolveVaryings(vec3 objectPosition, vec3 objectNormal, mat4 modelMatrix){
        v_noisePosition = (modelMatrix * vec4(objectPosition, 1.0)).xyz;
        v_noiseNormal = mat3(modelMatrix) * objectNormal;
    }
#endif
