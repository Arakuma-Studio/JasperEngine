// Final colour of library shaders: every library shader writes its output through
// finalizeColor, which applies the scene fog (HAS_FOG, routed by the blend mode define) and
// the linear -> display encoding the engine uses (the colour targets are RGBA8 holding
// sRGB-encoded colour). Later phases extend this function (tonemap, grade, vignette,
// dither) without touching the shaders that call it. Both follow the blend mode the variant
// draws with (BLEND_*): finalizeColor takes a colour as its texture stores it (premultiplied
// for the premultiplied blend), finalizeStraightColor a straight-alpha colour the shader
// computed (it premultiplies it for that blend).

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
// BLEND_PREMULTIPLIED, BLEND_ADDITIVE or BLEND_MULTIPLY. finalizeColor shapes the colour for
// it, as stored: alpha, opaque and additive take it as it is (additive adds rgb * alpha,
// its fog fades to black); premultiplied takes a colour already multiplied by its alpha (a
// premultiplied texture) and mixes the fog in times alpha; multiply fades the colour to
// white by its alpha (alpha 1 out), so the transparent parts leave what is behind
// unchanged, and its fog fades to white.
vec4 finalizeColor(vec4 linearColor){
    #if defined(BLEND_ADDITIVE)
        return finalizeBlendedColor(linearColor, vec3(0.0));
    #elif defined(BLEND_MULTIPLY)
        return finalizeBlendedColor(vec4(mix(vec3(1.0), linearColor.rgb, linearColor.a), 1.0), vec3(1.0));
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

// finalizeColor for a straight-alpha colour a shader computes (rgb not multiplied by its
// alpha: a procedural glow, a texture's colour times a fading alpha): the same as
// finalizeColor, except that the premultiplied blend gets the fogged display colour times
// alpha, so it draws exactly what the alpha blend draws (and a premultiplied texture the
// shader un-premultiplied comes back as it was).
vec4 finalizeStraightColor(vec4 linearColor){
    #if defined(BLEND_PREMULTIPLIED)
        vec3 color = linearColor.rgb;
        #ifdef HAS_FOG
            color = getFogColor(color);
        #endif
        return vec4(linearTosRGB(max(color, vec3(0.0))) * linearColor.a, linearColor.a);
    #else
        return finalizeColor(linearColor);
    #endif
}

// The scene fog on a linear colour drawn at this fragment (unchanged without HAS_FOG), for
// a shader that composites its own colour with one that already has the fog in it (the
// scene colour of includes/scene.glsl) and writes the result with finalizeFoggedColor.
// The fog is affine in the colour, so mix(scene, applyFog(own), a) is what the alpha blend
// of the fogged own colour over the scene draws.
vec3 applyFog(vec3 linearColor){
    #ifdef HAS_FOG
        return getFogColor(linearColor);
    #else
        return linearColor;
    #endif
}

// finalizeStraightColor for a straight-alpha colour whose fog is already in it (applyFog,
// the scene colour): the same shaping for the blend and the same encoding, no fog added
vec4 finalizeFoggedColor(vec4 linearColor){
    #if defined(BLEND_MULTIPLY)
        vec3 color = mix(vec3(1.0), linearColor.rgb, linearColor.a);
        return vec4(linearTosRGB(max(color, vec3(0.0))), 1.0);
    #elif defined(BLEND_PREMULTIPLIED)
        return vec4(linearTosRGB(max(linearColor.rgb, vec3(0.0))) * linearColor.a, linearColor.a);
    #else
        return vec4(linearTosRGB(max(linearColor.rgb, vec3(0.0))), linearColor.a);
    #endif
}

#endif
