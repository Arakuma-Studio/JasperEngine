// Scene fog, fragment stage only, with HAS_FOG. Its values come from the block the shader
// already uploads: the built-in Lit's camera block (includes/lit_frame.glsl), else the
// engine frame block when the fragment stage declares it before this include
// (includes/frame.glsl: fogColor_type and fogDensity_start_end), else a 32-byte block of
// its own (u_fs_fog), so a shader that reads nothing else of the frame block (an unlit
// sprite, a tilemap) does not upload it for fog.
//
// The height fog (Fog type Height) is a variant of its own, HAS_HEIGHT_FOG with HAS_FOG: the
// same functions then evaluate includes/height_fog.glsl, colour * T + S along the view ray,
// from its block (u_fs_heightFog) instead of the distance fog's.
#ifndef FOG_GLSL
#define FOG_GLSL

#ifdef HAS_HEIGHT_FOG

#include "includes/height_fog.glsl"

// The height fog on a colour at the surface under this fragment whose fog distance (below)
// is fogDist: a shader drawing onto another surface (a projected decal) passes that surface's
vec3 getFogColorAtDistance(vec3 color, float fogDist){
    return heightFogApply(color, gl_FragCoord.xy, heightFogPositionAtFogDistance(gl_FragCoord.xy, fogDist));
}

// The height fog on a colour drawn at this fragment
vec3 getFogColor(vec3 color){
    return heightFogApply(color, gl_FragCoord.xy, heightFogWorldPosition(gl_FragCoord.xy, gl_FragCoord.z));
}

// The fog factor at a fog distance (1 no fog .. 0 all fog colour): the height fog's
// transmittance there
float getFogFactor(float fogDist){
    return getFogColorAtDistance(vec3(1.0), fogDist).r - getFogColorAtDistance(vec3(0.0), fogDist).r;
}

#else

#if defined(LIT_FRAME_DECLARED)
    // one parameter serves density (exponential) or start (linear)
    #define FOG_COLOR_TYPE vec4(litFrame.fogColor_param.rgb, litFrame.ambient_fogType.w)
    #define FOG_DENSITY_START_END vec4(litFrame.fogColor_param.w, 0.0, litFrame.fogColor_param.w, litFrame.cameraPosition_fogEnd.w)
#elif defined(FRAME_FS_DECLARED)
    #define FOG_COLOR_TYPE frame.fogColor_type
    #define FOG_DENSITY_START_END frame.fogDensity_start_end
#else
    uniform u_fs_fog {
        vec4 color_type;         // rgb fog colour, w type: 0 linear, 1 exponential, 2 exponential squared
        vec4 density_start_end;  // x density, z linear start, w linear end
    } engineFog;
    #define FOG_COLOR_TYPE engineFog.color_type
    #define FOG_DENSITY_START_END engineFog.density_start_end
#endif

// The fog factor at a fog distance (1 no fog .. 0 all fog colour). The fog distance of a
// fragment is gl_FragCoord.z / gl_FragCoord.w: its window depth times its clip w (about the
// view distance in perspective, the 0..1 depth in orthographic cameras), what every built-in
// and library shader fogs by.
float getFogFactor(float fogDist){
    vec4 fogColorType = FOG_COLOR_TYPE;
    vec4 fogDensityStartEnd = FOG_DENSITY_START_END;
    int fogType = int(fogColorType.w);
    float fogDensity = fogDensityStartEnd.x;
    float fogLinearStart = fogDensityStartEnd.z;
    float fogLinearEnd = fogDensityStartEnd.w;

    float fogFactor = 0.0;
    const float LOG2 = 1.442695;
    if (fogType == 0){
        fogFactor = (fogLinearEnd - fogDist)/(fogLinearEnd - fogLinearStart);
    }else if (fogType == 1){
        fogFactor = exp2( -fogDensity * fogDist * LOG2);
    }else if (fogType == 2){
        fogFactor = exp2( -fogDensity * fogDensity * fogDist * fogDist * LOG2);
    }
    return clamp(fogFactor, 0.0, 1.0);
}

// The fog on a colour at a fog distance: a shader drawing onto another surface (a projected
// decal) passes that surface's, so both get the same fog
vec3 getFogColorAtDistance(vec3 color, float fogDist){
    return mix(FOG_COLOR_TYPE.xyz, color, getFogFactor(fogDist));
}

// The fog on a colour drawn at this fragment
vec3 getFogColor(vec3 color){
    return getFogColorAtDistance(color, gl_FragCoord.z / gl_FragCoord.w);
}

#endif // HAS_HEIGHT_FOG

#endif
