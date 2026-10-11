// The interpolation of the UV varyings v_uv1 and v_uv2 of the shared vertex path
// (includes/surface_vs.glsl), for the fragment stages that read them. With MSAA an edge pixel
// is shaded at its center, which can lie outside a sprite or tile quad: under
// HAS_TEXTURERECT (a sprite's or an instance's atlas rect) the vertex stage writes them
// `centroid`, which keeps the atlas UV inside the rect. GLSL ES 3.00 links a varying only
// when both stages give it the same qualifiers, so a fragment stage declares them with
// SURFACE_UV_IN, never a bare `in`:
//
//   #include "includes/surface_uv.glsl"
//   SURFACE_UV_IN vec2 v_uv1;
//
// (under whatever condition the shader reads them; the vertex path writes v_uv1 with
// SURFACE_UV1 and v_uv2 with SURFACE_UV2). surface_vs.glsl declares its outputs with
// SURFACE_UV_OUT from here, so the two cannot drift apart.

#ifndef SURFACE_UV_GLSL
#define SURFACE_UV_GLSL

#ifdef HAS_TEXTURERECT
    #define SURFACE_UV_OUT centroid out
    #define SURFACE_UV_IN centroid in
#else
    #define SURFACE_UV_OUT out
    #define SURFACE_UV_IN in
#endif

#endif
