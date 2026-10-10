// Matcap lookup for library fragment shaders (fake_lit, matcap). Declares the fragment
// frame block (includes/frame.glsl), so include it only in a .frag that reads it.
#ifndef MATCAP_GLSL
#define MATCAP_GLSL

#ifndef FRAME_FS
    #define FRAME_FS
#endif
#include "includes/frame.glsl"

// The matcap texture coordinate of a world normal seen at a world position: the view-space
// normal's xy, measured in a basis perpendicular to the ray from the camera instead of the
// view axes, so a perspective camera does not stretch the matcap at the screen's edges (with
// an orthographic camera the ray is the view axis and this is the plain n.xy * 0.5 + 0.5).
// The image's top (v = 0) is what faces up on screen.
vec2 matcapUV(vec3 worldNormal, vec3 worldPosition){
    vec3 n = normalize(mat3(frame.viewMatrix) * worldNormal);
    vec3 ray = vec3(0.0, 0.0, -1.0);
    if (frame.cameraPosition.w < 0.5){
        ray = normalize((frame.viewMatrix * vec4(worldPosition, 1.0)).xyz);
    }
    vec3 right = normalize(vec3(-ray.z, 0.0, ray.x));
    vec3 up = cross(right, ray);
    // 0.495: keeps the lookup off the image's border
    return vec2(0.5 + 0.495 * dot(right, n), 0.5 - 0.495 * dot(up, n));
}

#endif
