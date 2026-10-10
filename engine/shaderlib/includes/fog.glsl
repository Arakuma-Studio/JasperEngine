// Scene fog. Its values live in the engine frame block (frame.glsl: fogColor_type and
// fogDensity_start_end), declared here in the fragment stage, so fog costs no uniform
// block of its own. Include it only in a .frag, and only with HAS_FOG.
#ifndef FOG_GLSL
#define FOG_GLSL

#ifndef FRAME_FS
    #define FRAME_FS
#endif
#include "includes/frame.glsl"

vec3 getFogColor(vec3 color){
    int fogType = int(frame.fogColor_type.w);
    vec3 fogColor = frame.fogColor_type.xyz;
    float fogDensity = frame.fogDensity_start_end.x;
    float fogLinearStart = frame.fogDensity_start_end.z;
    float fogLinearEnd = frame.fogDensity_start_end.w;

    float fogFactor = 0.0;
    const float LOG2 = 1.442695;
    float fogDist = (gl_FragCoord.z / gl_FragCoord.w);
    if (fogType == 0){
        fogFactor = (fogLinearEnd - fogDist)/(fogLinearEnd - fogLinearStart);
    }else if (fogType == 1){
        fogFactor = exp2( -fogDensity * fogDist * LOG2);
    }else if (fogType == 2){
        fogFactor = exp2( -fogDensity * fogDensity * fogDist * fogDist * LOG2);
    }
    fogFactor = clamp(fogFactor, 0.0, 1.0);

    return mix(fogColor, color, fogFactor);
}

#endif
