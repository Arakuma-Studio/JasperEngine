#version 450

// The Title Art Renderer's Image: the shared UI vertex path, and with ANIM (S85 R13) the
// title's motion: each vertex moved by a 2x3 affine in frame-normalised space (0..1 over the
// Image, y down), so a scaled, rotated or slid title and a flipbook frame larger than the still
// are never cut at the Image's rect. animSpace maps the element's own space onto that space.

#ifdef ANIM
    #define UI_POSITION_HOOK
    uniform u_vs_customParams {
        vec4 animVertex0;
        vec4 animVertex1;
        vec4 animSpace;     // xy: the element-space origin of the frame, zw: its size
    } customParams;
#endif

#include "includes/ui_vs.glsl"

#ifdef ANIM
    vec3 uiPositionHook(vec3 position){
        vec2 size = customParams.animSpace.zw;
        vec2 n = (position.xy - customParams.animSpace.xy) / vec2(abs(size.x) > 1e-6 ? size.x : 1.0, abs(size.y) > 1e-6 ? size.y : 1.0);
        vec3 q = vec3(n, 1.0);
        vec2 moved = vec2(dot(customParams.animVertex0.xyz, q), dot(customParams.animVertex1.xyz, q));
        return vec3(customParams.animSpace.xy + moved * size, position.z);
    }
#endif
