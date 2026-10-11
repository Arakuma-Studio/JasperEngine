// Final colour of library shaders: every library shader writes its output through
// finalizeColor, which applies the scene fog (HAS_FOG, routed by the blend mode define) and
// the linear -> display encoding the engine uses (the colour targets are RGBA8 holding
// sRGB-encoded colour). Later phases extend this function (tonemap, grade, vignette,
// dither) without touching the shaders that call it.

#ifndef OUTPUT_GLSL
#define OUTPUT_GLSL

#include "includes/srgb.glsl"

#ifdef HAS_FOG
    #include "includes/fog.glsl"
#endif

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

// The engine defines the blend mode the variant's pipeline resolves to (material or
// component, else @state, else the legacy alpha blend): BLEND_NONE, BLEND_ALPHA,
// BLEND_PREMULTIPLIED, BLEND_ADDITIVE or BLEND_MULTIPLY. finalizeColor fogs by it:
// additive fades to black, multiply to white (finalizeBlendedColor), premultiplied mixes
// towards the fog colour times alpha (the colour is already multiplied by it).
vec4 finalizeColor(vec4 linearColor){
    #if defined(BLEND_ADDITIVE)
        return finalizeBlendedColor(linearColor, vec3(0.0));
    #elif defined(BLEND_MULTIPLY)
        return finalizeBlendedColor(linearColor, vec3(1.0));
    #else
        vec3 color = linearColor.rgb;
        #ifdef HAS_FOG
            #ifdef BLEND_PREMULTIPLIED
                // mix(fogColor * a, color, factor): getFogColor(0) is (1 - factor) * fogColor
                vec3 fogFactor = getFogColor(vec3(1.0)) - getFogColor(vec3(0.0));
                color = getFogColor(vec3(0.0)) * linearColor.a + fogFactor * color;
            #else
                color = getFogColor(color);
            #endif
        #endif
        return vec4(linearTosRGB(max(color, vec3(0.0))), linearColor.a);
    #endif
}

#endif
