// Engine planar reflection for library shaders (manifest `@require planarreflection`, mesh
// shaders, fragment stage). Include it in the .frag after declaring your blocks. Without the
// define (SURFACE_PLANAR_REFLECTION, which the require adds) it declares nothing.
//
// The reflection is the scene mirrored about the horizontal plane at the draw's water level
// (the top of the mesh's world bounds), drawn by a pass of the camera for each such height
// (RenderSystem's water reflection passes, the scene's Planar Reflections settings). A draw
// without one this frame (the scene's resolution Off, more heights than it reflects, a camera
// image or a reflection probe face, the eye under the plane) is told so: planarReflectionPresent()
// is false, and the shader shows its fallback (library:water: the environment).
//
//   bool planarReflectionPresent();
//   float planarReflectionLevel();                                    // the plane's world Y
//   vec3 planarReflectionColor(vec3 worldPosition, vec2 screenShift); // linear
//
// worldPosition is a point of the surface (it is flattened onto the plane: the waves keep the
// reflection where the flat surface has it), screenShift a distortion in screen UV (the ripples).
//
// Engine data (RenderSystem::applyPlanarReflection):
//   u_fs_planarReflection.viewProjection: the reflection camera's (logical: the shader handles
//                                         the texture origin, as mesh.frag's mirror)
//   u_fs_planarReflection.params:         x the plane's height, y 1 with a reflection, 0 without
//   u_planarReflectionTexture:            its colour (display-encoded, decoded with sRGBToLinear)
// With `@require environment` as well, the two members are the last of u_fs_environment
// (planarViewProjection, planarParams; includes/environment.glsl) instead of a block of their own.

#ifndef PLANAR_REFLECTION_GLSL
#define PLANAR_REFLECTION_GLSL

#ifdef SURFACE_PLANAR_REFLECTION

#include "includes/srgb.glsl"

#ifdef SURFACE_ENVIRONMENT
    #include "includes/environment.glsl"
    #define PLANAR_REFLECTION_VIEW_PROJECTION environment.planarViewProjection
    #define PLANAR_REFLECTION_PARAMS environment.planarParams
#else
    uniform u_fs_planarReflection {
        mat4 viewProjection;
        vec4 params;
    } planarReflection;
    #define PLANAR_REFLECTION_VIEW_PROJECTION planarReflection.viewProjection
    #define PLANAR_REFLECTION_PARAMS planarReflection.params
#endif

uniform texture2D u_planarReflectionTexture;
uniform sampler u_planarReflection_smp;

bool planarReflectionPresent(){
    return PLANAR_REFLECTION_PARAMS.y > 0.5;
}

float planarReflectionLevel(){
    return PLANAR_REFLECTION_PARAMS.x;
}

vec3 planarReflectionColor(vec3 worldPosition, vec2 screenShift){
    vec4 clip = PLANAR_REFLECTION_VIEW_PROJECTION * vec4(worldPosition.x, PLANAR_REFLECTION_PARAMS.x, worldPosition.z, 1.0);
    // y flipped for the top-left framebuffer origin (all backends), as mesh.frag's mirror
    vec2 uv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) + vec2(screenShift.x, -screenShift.y);
    uv = clamp(uv, vec2(0.001), vec2(0.999));
    return sRGBToLinear(textureLod(sampler2D(u_planarReflectionTexture, u_planarReflection_smp), uv, 0.0)).rgb;
}

#endif // SURFACE_PLANAR_REFLECTION

#endif // PLANAR_REFLECTION_GLSL
