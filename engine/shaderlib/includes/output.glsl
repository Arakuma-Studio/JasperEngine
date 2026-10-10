// Final colour of library shaders: every library shader writes its output through
// finalizeColor, which applies the scene fog (HAS_FOG) and the linear -> display encoding
// the engine uses (the colour targets are RGBA8 holding sRGB-encoded colour). Later
// phases extend this function (tonemap, grade, vignette, dither) without touching the
// shaders that call it.

#ifndef OUTPUT_GLSL
#define OUTPUT_GLSL

#include "includes/srgb.glsl"

#ifdef HAS_FOG
    #include "includes/fog.glsl"
#endif

vec4 finalizeColor(vec4 linearColor){
    vec3 color = linearColor.rgb;
    #ifdef HAS_FOG
        color = getFogColor(color);
    #endif
    return vec4(linearTosRGB(max(color, vec3(0.0))), linearColor.a);
}

// finalizeColor for a colour a blend adds to the scene (additive: neutral black) or
// multiplies it by (multiply: neutral white): the fog fades it to the neutral colour, which
// leaves the scene behind unchanged (that is fogged already), instead of mixing it towards
// the fog colour, which an additive blend would add on top
vec4 finalizeBlendedColor(vec4 linearColor, vec3 neutral){
    vec3 color = linearColor.rgb;
    #ifdef HAS_FOG
        // getFogColor is mix(fogColor, color, factor): its factor, per channel
        vec3 fogFactor = getFogColor(vec3(1.0)) - getFogColor(vec3(0.0));
        color = mix(neutral, color, fogFactor);
    #endif
    return vec4(linearTosRGB(max(color, vec3(0.0))), linearColor.a);
}

#endif
