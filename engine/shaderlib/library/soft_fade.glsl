// The soft fades of library:particle, library:unlit and library:depth_fade (fragment stage;
// include it only in variants that fade, after the parameter block):
//
// - softIntersection(softness): 0 where the surface touches the opaque scene behind it, 1
//   from softness world units in front of it (the scene depth minus the fragment's depth,
//   includes/scene.glsl), so a quad crossing the ground or a wall shows no hard line. Needs
//   @require scenedepth for the variant (SURFACE_SCENE_DEPTH).
// - softViewDistance(): the fragment's view-space distance (world units along the camera's
//   axis, from the camera plane), for the camera fades. With a scene input it is
//   fragmentLinearDepth(); without one it comes from the frame block's projection and
//   gl_FragCoord.z, so a camera fade alone reads no scene depth (no depth pre-pass).
// - softRamp(value, from, to): 0 at from, 1 at to, smooth in between; from > to ramps
//   the other way (a far fade); from == to is a step.
//
// The frame-block distance inverts the z and w rows of the projection, which a standard
// perspective or orthographic projection keeps free of x and y (a mirror's oblique near
// plane is not: there it is approximate, which a fade tolerates); the target's Y flip only
// touches row y. gl_FragCoord.z is the window depth, 0 near .. 1 far, on every backend.

#ifndef SOFT_FADE_GLSL
#define SOFT_FADE_GLSL

#if defined(SURFACE_SCENE_DEPTH) || defined(SURFACE_SCENE_COLOR)
    #include "includes/scene.glsl"
#else
    #ifndef FRAME_FS
        #define FRAME_FS
    #endif
    #include "includes/frame.glsl"
#endif

float softViewDistance(){
    #if defined(SURFACE_SCENE_DEPTH) || defined(SURFACE_SCENE_COLOR)
        return fragmentLinearDepth();
    #else
        // clip z = P[2][2] z + P[3][2], clip w = P[2][3] z + P[3][3] (view z negative in front)
        float ndc = gl_FragCoord.z * 2.0 - 1.0;
        mat4 p = frame.projectionMatrix;
        return (ndc * p[3][3] - p[3][2]) / (ndc * p[2][3] - p[2][2]);
    #endif
}

float softRamp(float value, float from, float to){
    float range = to - from;
    // a zero range is a step at from (away from zero keeps the sign of the direction)
    float t = clamp((value - from) / (abs(range) < 0.0001 ? (range < 0.0 ? -0.0001 : 0.0001) : range), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

#ifdef SURFACE_SCENE_DEPTH
    float softIntersection(float softness){
        float gap = sceneLinearDepth(screenUV()) - fragmentLinearDepth();
        return clamp(gap / max(softness, 0.0001), 0.0, 1.0);
    }
#endif

#endif
