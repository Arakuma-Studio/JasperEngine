#version 450

// The Title Art Renderer's Sprite: the library vertex path, and with ANIM (S85 R13) the title's
// motion as title_fx.vert moves an Image: a 2x3 affine in frame-normalised space (0..1 over
// the sprite, y down from its top), animSpace mapping the sprite's object space onto it. The
// shader has no depth or shadow programs (zwrite off, shadows off), so nothing else must match.

#ifdef ANIM
    #define SURFACE_DISPLACE
    uniform u_vs_customParams {
        vec4 animVertex0;
        vec4 animVertex1;
        vec4 animSpace;     // xy: the object-space origin of the frame's top-left, zw: its size (y negative: up)
    } customParams;
#endif

#include "includes/surface_vs.glsl"

#ifdef ANIM
    vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
        vec2 size = customParams.animSpace.zw;
        vec2 n = (objectPosition.xy - customParams.animSpace.xy) / vec2(abs(size.x) > 1e-6 ? size.x : 1.0, abs(size.y) > 1e-6 ? size.y : 1.0);
        vec3 q = vec3(n, 1.0);
        vec2 moved = vec2(dot(customParams.animVertex0.xyz, q), dot(customParams.animVertex1.xyz, q));
        return vec3(customParams.animSpace.xy + moved * size, objectPosition.z);
    }
#endif
