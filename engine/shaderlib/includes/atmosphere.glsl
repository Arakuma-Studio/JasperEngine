// What the atmosphere passes (volumetric fog, clouds and their composites, shaderlib/
// atmosphere_*.frag, volumetric_*.frag, sky_composite.frag) share: the orientation of their
// targets, the static blue-noise jitter, the packing of (scattering, transmittance) into
// their targets and the camera rays.
//
// Targets: an atmosphere target's texel at camera UV (0, 0 bottom left, y up) is where a
// fullscreen pass writes the fragment of that UV: GL stores row 0 at the bottom, the other
// backends at the top, so the UV flips on them (the same rule as the depth pre-pass,
// includes/depth_util.glsl). atmosphereTargetUV converts both ways.
//
// Scattering S (linear radiance) and transmittance T go into RGBA16F targets as they are, or
// into RGBA8 ones (no float colour targets: GLES3/WebGL2 without EXT_color_buffer_float) as
// S / (1 + S), decoded by the reader (docs/specs/S12-atmosphere.md R14).
//
// Every jitter is static per pixel (a 64x64 blue-noise map, no frame index): two frames of a
// static scene are identical.

#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

// the camera UV of an atmosphere target's texel UV, and back
vec2 atmosphereTargetUV(vec2 uv){
    #ifdef IS_GLSL
        return uv;
    #else
        return vec2(uv.x, 1.0 - uv.y);
    #endif
}

// the camera UV of this fragment of a fullscreen pass into a target of 1 / size invSize
vec2 atmosphereFragmentUV(vec2 invSize){
    return atmosphereTargetUV(gl_FragCoord.xy * invSize);
}

// S and T into a target texel: compressed (RGBA8 targets) or as they are
vec4 atmospherePack(vec3 scattering, float transmittance, float compressed){
    vec3 s = max(scattering, vec3(0.0));
    if (compressed > 0.5)
        s = s / (vec3(1.0) + s);
    return vec4(s, clamp(transmittance, 0.0, 1.0));
}

void atmosphereUnpack(vec4 texel, float compressed, out vec3 scattering, out float transmittance){
    vec3 s = texel.rgb;
    if (compressed > 0.5)
        s = s / max(vec3(1.0) - s, vec3(1.0 / 255.0));
    scattering = s;
    transmittance = texel.a;
}

// the static jitter of this fragment, 0..1 (a pass declaring ATMOSPHERE_BLUE_NOISE before the
// include binds u_blueNoise: R8, nearest, repeat)
#ifdef ATMOSPHERE_BLUE_NOISE
    uniform texture2D u_blueNoise;
    uniform sampler u_blueNoise_smp;

    float atmosphereBlueNoise(){
        return textureLod(sampler2D(u_blueNoise, u_blueNoise_smp), gl_FragCoord.xy / 64.0, 0.0).r;
    }
#endif

#ifdef OUTPUT_GLSL
// The display colour of a layer of fog or cloud for the composites' premultiplied blend
// (src ONE, dst ONE_MINUS_SRC_ALPHA): what is behind it times T, plus its own colour times
// its opacity 1 - T. The colour targets hold display-encoded colour, so the layer is blended
// as a transparent surface of colour S / (1 - T) is (an exact composite for T 0 or 1, as the
// engine's transparent draws). encodeOutput gives it the post-processing colour (include
// includes/output.glsl first).
vec4 atmosphereLayerColor(vec3 scattering, float transmittance){
    float opacity = clamp(1.0 - transmittance, 0.0, 1.0);
    if (opacity < 1.0 / 1024.0)
        return vec4(0.0);
    return vec4(encodeOutput(scattering / opacity) * opacity, opacity);
}
#endif

// The view ray of a camera UV: origin (the eye, or the near plane of an orthographic camera)
// and unit direction, from the camera's inverse view-projection (camera clip -> world)
void atmosphereCameraRay(mat4 invViewProjection, vec2 cameraUV, out vec3 origin, out vec3 dir){
    vec2 ndc = cameraUV * 2.0 - 1.0;
    vec4 nearPoint = invViewProjection * vec4(ndc, -1.0, 1.0);
    vec4 farPoint = invViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 a = nearPoint.xyz / nearPoint.w;
    vec3 b = farPoint.xyz / farPoint.w;
    vec4 eye = invViewProjection[2];
    origin = (abs(eye.w) > 1e-7) ? eye.xyz / eye.w : a;
    dir = normalize(b - a);
}

#endif
