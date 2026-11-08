// Engine reflection environment for library shaders (manifest `@require environment`, mesh
// shaders, fragment stage). Include it in the .frag after declaring your blocks. Without the
// define (SURFACE_ENVIRONMENT, which the require adds) it declares nothing.
//
// The environment is what the built-in Lit shader reflects: the scene sky's prefiltered
// (GGX) environment map, turned and tinted as the sky lights the scene, or, for an object
// inside a reflection probe's box, that probe's capture (no box projection here). Without
// either (no sky texture, its maps still being made) it is black and environmentPresent()
// is false: fall back to a colour of your own then.
//
//   vec3 environmentColor(vec3 worldDirection, float roughness); // linear, roughness 0..1
//   bool environmentPresent();
//
// Engine data (RenderSystem::applyEnvironment):
//   u_fs_environment.colorRotation: rgb linear tint (the sky colour, a probe's intensity),
//                                   w rotation about +Y in radians (0 for a probe)
//   u_fs_environment.params:        x the map's highest mip level (roughness 1 samples it),
//                                   y 1 with an environment, 0 without
//   u_environmentTexture:           the cube map (display-encoded, decoded with sRGBToLinear)
//   with @require planarreflection too, the planar reflection's block follows in this one

#ifndef ENVIRONMENT_GLSL
#define ENVIRONMENT_GLSL

#ifdef SURFACE_ENVIRONMENT

#include "includes/srgb.glsl"

uniform u_fs_environment {
    vec4 colorRotation;
    vec4 params;
    #ifdef SURFACE_PLANAR_REFLECTION
        // the planar reflection's (includes/planar_reflection.glsl), in this block when a shader
        // reads both: one uniform block of the fragment stage's eight for the two
        mat4 planarViewProjection;
        vec4 planarParams;
    #endif
} environment;

uniform textureCube u_environmentTexture;
uniform sampler u_environment_smp;

bool environmentPresent(){
    return environment.params.y > 0.5;
}

vec3 environmentColor(vec3 worldDirection, float roughness){
    float c = cos(environment.colorRotation.w);
    float s = sin(environment.colorRotation.w);
    vec3 d = vec3(c * worldDirection.x - s * worldDirection.z, worldDirection.y, s * worldDirection.x + c * worldDirection.z);
    float lod = clamp(roughness, 0.0, 1.0) * environment.params.x;
    return sRGBToLinear(textureLod(samplerCube(u_environmentTexture, u_environment_smp), d, lod)).rgb * environment.colorRotation.rgb;
}

#endif // SURFACE_ENVIRONMENT

#endif // ENVIRONMENT_GLSL
