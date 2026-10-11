// Scene fog, fragment stage only, with HAS_FOG. Its values come from the block the shader
// already uploads: the built-in Lit's camera block (includes/lit_frame.glsl), else the
// engine frame block when the fragment stage declares it before this include
// (includes/frame.glsl: fogColor_type and fogDensity_start_end), else a 32-byte block of
// its own (u_fs_fog), so a shader that reads nothing else of the frame block (an unlit
// sprite, a tilemap) does not upload it for fog.
#ifndef FOG_GLSL
#define FOG_GLSL

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

vec3 getFogColor(vec3 color){
    vec4 fogColorType = FOG_COLOR_TYPE;
    vec4 fogDensityStartEnd = FOG_DENSITY_START_END;
    int fogType = int(fogColorType.w);
    vec3 fogColor = fogColorType.xyz;
    float fogDensity = fogDensityStartEnd.x;
    float fogLinearStart = fogDensityStartEnd.z;
    float fogLinearEnd = fogDensityStartEnd.w;

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
