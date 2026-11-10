// Final colour of library shaders: every library shader writes its output through
// finalizeColor, which applies the scene fog (HAS_FOG, routed by the blend mode define) and
// the linear -> display encoding the engine uses (the colour targets are RGBA8 holding
// sRGB-encoded colour). With the post-processing colour (phase 4, USE_POST_COLOR) the
// encoding also applies the exposure, tone mapping, grade, vignette and dither
// (includes/post_color.glsl): encodeOutput for a shader's own colour, encodeAdditiveOutput for
// one a blend adds; a filter a blend multiplies by, and the scene colour (graded already),
// keep the plain encodeColor. Both follow the blend mode the variant
// draws with (BLEND_*): finalizeColor takes a colour as its texture stores it (premultiplied
// for the premultiplied blend), finalizeStraightColor a straight-alpha colour the shader
// computed (it premultiplies it for that blend).

#ifndef OUTPUT_GLSL
#define OUTPUT_GLSL

#include "includes/srgb.glsl"
#include "includes/post_color.glsl"

#ifdef HAS_FOG
    #include "includes/fog.glsl"
#endif

// A library shader on a terrain with holes (T3 R20): every colour leaves through the finalizers
// below, so a shader of the library needs no line of its own for holes
#ifdef TERRAIN_HOLES
    #include "includes/terrain_holes_fs.glsl"
    #define OUTPUT_TERRAIN_HOLE() terrainDiscardHole()
#else
    #define OUTPUT_TERRAIN_HOLE()
#endif

// The display encoding of a shader's own linear colour (clamped at 0): with the
// post-processing colour, its exposure, tone mapping, grade, vignette and dither
vec3 encodeOutput(vec3 linearColor){
    #ifdef USE_POST_COLOR
        return postColorEncode(linearColor);
    #else
        return linearTosRGB(max(linearColor, vec3(0.0)));
    #endif
}

// encodeOutput for a colour a blend adds to the target (black adds nothing; the vignette only
// darkens it)
vec3 encodeAdditiveOutput(vec3 linearColor){
    #ifdef USE_POST_COLOR
        return postColorEncodeAdditive(linearColor);
    #else
        return linearTosRGB(max(linearColor, vec3(0.0)));
    #endif
}

// The fog distance of this fragment (fog.glsl), which finalizeColor and the others fog by;
// the ...AtFogDistance forms take another one: a shader drawing onto another surface (a
// projected decal) passes that surface's (sceneFogDistance below), so the colour it lays on
// it gets the fog the surface got
float fragmentFogDistance(){
    #ifdef HAS_FOG
        return gl_FragCoord.z / gl_FragCoord.w;
    #else
        return 0.0;
    #endif
}

// A colour that blends with what is behind it (a glint the blend adds: neutral black; a
// filter the blend multiplies by: neutral white) faded by the fog towards the neutral
// colour, which leaves the scene behind as it is (that is fogged already) instead of mixing
// it towards the fog colour. Unchanged without HAS_FOG.
vec3 fadeByFogAtFogDistance(vec3 linearColor, vec3 neutral, float fogDistance){
    #ifdef HAS_FOG
        // getFogColorAtDistance is mix(fogColor, color, factor): its factor, per channel
        vec3 factor = getFogColorAtDistance(vec3(1.0), fogDistance) - getFogColorAtDistance(vec3(0.0), fogDistance);
        return mix(neutral, linearColor, factor);
    #else
        return linearColor;
    #endif
}

vec3 fadeByFog(vec3 linearColor, vec3 neutral){
    return fadeByFogAtFogDistance(linearColor, neutral, fragmentFogDistance());
}

// finalizeColor for a colour a blend adds to the scene (additive: neutral black) or
// multiplies it by (multiply: neutral white): the fog fades it to the neutral colour
// (fadeByFog), instead of mixing it towards the fog colour, which an additive blend would
// add on top
vec4 finalizeBlendedColorAtFogDistance(vec4 linearColor, vec3 neutral, float fogDistance){
    OUTPUT_TERRAIN_HOLE();
    vec3 color = fadeByFogAtFogDistance(linearColor.rgb, neutral, fogDistance);
    // a colour added to the target takes the post-processing colour as an added one; a filter
    // the target is multiplied by stays a plain factor (the target is graded already)
    #ifdef USE_POST_COLOR
        if (neutral.r < 0.5)
            return vec4(encodeAdditiveOutput(color), linearColor.a);
    #endif
    return vec4(linearTosRGB(max(color, vec3(0.0))), linearColor.a);
}

vec4 finalizeBlendedColor(vec4 linearColor, vec3 neutral){
    return finalizeBlendedColorAtFogDistance(linearColor, neutral, fragmentFogDistance());
}

// The engine defines the blend mode the variant's pipeline resolves to (material or
// component, else @state, else the legacy alpha blend): BLEND_NONE, BLEND_ALPHA,
// BLEND_PREMULTIPLIED, BLEND_ADDITIVE or BLEND_MULTIPLY. finalizeColor shapes the colour for
// it, as stored: alpha, opaque and additive take it as it is (additive adds rgb * alpha,
// its fog fades to black); premultiplied takes a colour already multiplied by its alpha (a
// premultiplied texture) and mixes the fog in times alpha; multiply fades the colour to
// white by its alpha (alpha 1 out), so the transparent parts leave what is behind
// unchanged, and its fog fades to white.
vec4 finalizeColorAtFogDistance(vec4 linearColor, float fogDistance){
    OUTPUT_TERRAIN_HOLE();
    #if defined(BLEND_ADDITIVE)
        return finalizeBlendedColorAtFogDistance(linearColor, vec3(0.0), fogDistance);
    #elif defined(BLEND_MULTIPLY)
        return finalizeBlendedColorAtFogDistance(vec4(mix(vec3(1.0), linearColor.rgb, linearColor.a), 1.0), vec3(1.0), fogDistance);
    #else
        vec3 color = linearColor.rgb;
        #ifdef HAS_FOG
            #ifdef BLEND_PREMULTIPLIED
                // mix(fogColor * a, color, factor): the fog of black is (1 - factor) * fogColor
                vec3 fogOfBlack = getFogColorAtDistance(vec3(0.0), fogDistance);
                vec3 factor = getFogColorAtDistance(vec3(1.0), fogDistance) - fogOfBlack;
                color = fogOfBlack * linearColor.a + factor * color;
            #else
                color = getFogColorAtDistance(color, fogDistance);
            #endif
        #endif
        #if defined(BLEND_PREMULTIPLIED) && defined(USE_POST_COLOR)
            // the curve and grade of the straight colour, premultiplied again
            float coverage = max(linearColor.a, 1e-4);
            return vec4(encodeOutput(color / coverage) * linearColor.a, linearColor.a);
        #else
            return vec4(encodeOutput(color), linearColor.a);
        #endif
    #endif
}

vec4 finalizeColor(vec4 linearColor){
    return finalizeColorAtFogDistance(linearColor, fragmentFogDistance());
}

// finalizeColor for a straight-alpha colour a shader computes (rgb not multiplied by its
// alpha: a procedural glow, a texture's colour times a fading alpha): the same as
// finalizeColor, except that the premultiplied blend gets the fogged display colour times
// alpha, so it draws exactly what the alpha blend draws (and a premultiplied texture the
// shader un-premultiplied comes back as it was).
vec4 finalizeStraightColorAtFogDistance(vec4 linearColor, float fogDistance){
    OUTPUT_TERRAIN_HOLE();
    #if defined(BLEND_PREMULTIPLIED)
        vec3 color = linearColor.rgb;
        #ifdef HAS_FOG
            color = getFogColorAtDistance(color, fogDistance);
        #endif
        return vec4(encodeOutput(color) * linearColor.a, linearColor.a);
    #else
        return finalizeColorAtFogDistance(linearColor, fogDistance);
    #endif
}

vec4 finalizeStraightColor(vec4 linearColor){
    return finalizeStraightColorAtFogDistance(linearColor, fragmentFogDistance());
}

// The scene fog on a linear colour drawn at this fragment (unchanged without HAS_FOG), for
// a shader that composites its own colour with one that already has the fog in it (the
// scene colour of includes/scene.glsl) and writes the result with finalizeFoggedColor.
vec3 applyFog(vec3 linearColor){
    #ifdef HAS_FOG
        return getFogColor(linearColor);
    #else
        return linearColor;
    #endif
}

// finalizeStraightColor for a straight-alpha colour whose fog is already in it (applyFog,
// the scene colour): the same shaping for the blend and the same encoding, no fog added. No
// post-processing colour either: the scene colour has it already (a colour of the shader's
// own goes through encodeOutput)
vec4 finalizeFoggedColor(vec4 linearColor){
    OUTPUT_TERRAIN_HOLE();
    #if defined(BLEND_MULTIPLY)
        vec3 color = mix(vec3(1.0), linearColor.rgb, linearColor.a);
        return vec4(linearTosRGB(max(color, vec3(0.0))), 1.0);
    #elif defined(BLEND_PREMULTIPLIED)
        return vec4(linearTosRGB(max(linearColor.rgb, vec3(0.0))) * linearColor.a, linearColor.a);
    #else
        return vec4(linearTosRGB(max(linearColor.rgb, vec3(0.0))), linearColor.a);
    #endif
}

// Compositing over the scene colour. The colour targets hold display-encoded colour and the
// hardware blends those encoded values as stored, so a shader that mixes its own colour with
// a sample of the scene colour (includes/scene.glsl: linear, fogged) draws what the blend
// would only when it mixes encoded values: encodeColor them first (a multiply of encoded
// values is the encoding of the product, so filters may multiply either way). Mixing linear
// values instead brightens the mix (G 0.8 at alpha 0.5 over black: 0.66 against the
// blend's 0.45).
vec3 encodeColor(vec3 linearColor){
    return linearTosRGB(max(linearColor, vec3(0.0)));
}

// finalizeStraightColor of a straight-alpha colour drawn over a sample of the scene colour
// taken elsewhere than under the fragment (refraction), instead of over what the target
// holds: behindWeight 1 draws it over `behind` only (alpha 1 out: that replaces the
// target), 0 over the target exactly as finalizeStraightColor (bit for bit), in between a
// mix of the two backgrounds (alpha between). The alpha and premultiplied blends can draw
// that; the additive and multiply ones cannot remove what is behind, so they draw
// finalizeStraightColor (behind unused). Where the target is kept (weight < 1), transparent
// objects drawn before this one stay visible through it, which the scene colour (opaque
// scene and sky only) cannot show.
vec4 finalizeStraightColorOverScene(vec4 linearColor, vec3 behind, float behindWeight){
    OUTPUT_TERRAIN_HOLE();
    #if defined(BLEND_ADDITIVE) || defined(BLEND_MULTIPLY)
        return finalizeStraightColor(linearColor);
    #else
        vec3 own = encodeOutput(applyFog(linearColor.rgb));
        // a own + (1 - a) (w behind + (1 - w) target) is coverage c = a + (1 - a) w of
        // mix(own, behind, (1 - a) w / c) over the target
        float behindShare = (1.0 - linearColor.a) * behindWeight;
        float coverage = linearColor.a + behindShare;
        vec3 color = mix(own, encodeColor(behind), behindShare / max(coverage, 1e-6));
        #ifdef BLEND_PREMULTIPLIED
            return vec4(color * coverage, coverage);
        #else
            return vec4(color, coverage);
        #endif
    #endif
}

// The fog distance (fog.glsl) of the opaque surface under a screen UV whose stored depth is
// depth01 (sceneRawDepth), as that surface got it when it was drawn (gl_FragCoord.z /
// gl_FragCoord.w there: its depth times its clip w, the inverse projection's row w giving
// 1 / clip w), in perspective, orthographic and oblique projections. Include
// includes/scene.glsl first, in a variant reading the scene depth.
#if defined(SCENE_GLSL) && defined(SURFACE_SCENE_DEPTH)
    float sceneFogDistance(vec2 uv, float depth01){
        vec4 ndc = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
        return depth01 / dot(sceneInputs.inverseProjectionW, ndc);
    }
#endif

#endif
