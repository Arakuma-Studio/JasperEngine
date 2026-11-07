// Scene inputs of the shader library (phase 3): the opaque scene's depth and colour, for
// mesh and points fragment shaders. A shader asks for them in its manifest:
//
//   // @require scenedepth                 (or: @require scenedepth feature=SOFT)
//   // @require scenecolor
//
// which defines SURFACE_SCENE_DEPTH / SURFACE_SCENE_COLOR for the variants that need them,
// and makes the engine produce them in every camera pass that draws such a variant: the
// depth of the opaque queue (a depth pre-pass, shared with SSAO, SSR or a depth-reading
// post pass when one already runs) and one copy of the opaque queue and the sky (taken
// before the transparent queue, at the scene's Scene Color Scale). Include this file in the
// .frag only; without either define it declares nothing.
//
// Conventions (every backend, perspective and orthographic cameras, render-to-texture and
// fixed-resolution passes, editor views):
// - screen UV: (0, 0) at the bottom left of the camera's image, (1, 1) at the top right,
//   the y axis pointing up like normalized device coordinates. screenUV() is the current
//   fragment's; any other UV (screenUV() plus an offset) reads the inputs at that place.
// - depth: view-space distance along the camera's forward axis, in world units, from the
//   camera plane (not from the near plane). Where nothing opaque was drawn (sky, cleared
//   background) sceneLinearDepth is the far clip distance.
// - colour: linear (the copy holds the display-encoded opaque scene, fog included; this
//   decodes it). Transparent objects are not in it, so they do not see each other.
//
// The engine binds, by name: the uniform block u_fs_sceneInputs, u_sceneDepthTexture /
// u_sceneDepth_smp (packed RGBA8 depth, nearest filtering) and u_sceneColorTexture /
// u_sceneColor_smp (RGBA8, linear filtering). Drawn where no input exists (a draw in the
// opaque queue, which is a manifest/material error, or a reflection probe face), the depth
// reads as the far plane and the colour as black, and in a pass without a colour copy
// colorParams.zw are 0: a shader can test that and draw without the scene colour.

#ifndef SCENE_GLSL
#define SCENE_GLSL

#if defined(SURFACE_SCENE_DEPTH) || defined(SURFACE_SCENE_COLOR)

#include "includes/depth_util.glsl"
#include "includes/srgb.glsl"

// engine-written per camera pass (sceneinputs_t in RenderSystem.h)
uniform u_fs_sceneInputs {
    mat4 inverseViewProjection; // clip space -> world, the camera's unflipped matrices
    vec4 inverseProjectionZ;    // -(row z) of the inverse projection: distance = dot(z, p) / dot(w, p)
    vec4 inverseProjectionW;    // row w of the inverse projection (p = vec4(ndc, 1))
    vec4 screenParams;          // screenUV() = gl_FragCoord.xy * xy + zw
    vec4 colorParams;           // colour copy v = uv.y * x + y; zw = 1 / copy size in texels
    vec4 depthParams;           // depth texture uv = screen uv * xy + zw
    #ifdef SCENE_VIEW_PROJECTION
        // world -> clip space, the camera's unflipped matrices (the built-in Lit's transmission
        // refracts into the colour copy with it; a block without it is uploaded without it)
        mat4 viewProjection;
    #endif
} sceneInputs;

#ifdef SURFACE_SCENE_DEPTH
    uniform texture2D u_sceneDepthTexture;
    uniform sampler u_sceneDepth_smp;
#endif

#ifdef SURFACE_SCENE_COLOR
    uniform texture2D u_sceneColorTexture;
    uniform sampler u_sceneColor_smp;
#endif

// 0..1 screen position of the current fragment, y up (see above)
vec2 screenUV(){
    return gl_FragCoord.xy * sceneInputs.screenParams.xy + sceneInputs.screenParams.zw;
}

// view-space distance of a point given by its screen UV and its depth as the depth buffer
// stores it (0 near plane .. 1 far plane)
float sceneDistanceFromDepth(vec2 uv, float depth01){
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    return dot(sceneInputs.inverseProjectionZ, ndc) / dot(sceneInputs.inverseProjectionW, ndc);
}

// view-space distance of the current fragment (the surface being drawn)
float fragmentLinearDepth(){
    return sceneDistanceFromDepth(screenUV(), gl_FragCoord.z);
}

#ifdef SURFACE_SCENE_DEPTH
    // the opaque scene's depth at a screen UV as stored (0 near .. 1 far; 1 where nothing
    // opaque was drawn)
    float sceneRawDepth(vec2 uv){
        // the depth pre-pass target keeps the native row order (depth_util.glsl); a mirror's
        // is drawn mirrored left to right (its faces keep their winding)
        vec2 depthUV = uv * sceneInputs.depthParams.xy + sceneInputs.depthParams.zw;
        // the cleared background decodes slightly above 1
        return min(decodeDepth(textureLod(sampler2D(u_sceneDepthTexture, u_sceneDepth_smp), depthUV, 0.0)), 1.0);
    }

    // view-space distance of the opaque surface at a screen UV (the far clip distance where
    // there is none)
    float sceneLinearDepth(vec2 uv){
        return sceneDistanceFromDepth(uv, sceneRawDepth(uv));
    }

    // world position of the opaque surface at a screen UV (on the far plane where there is none)
    vec3 sceneWorldPosition(vec2 uv){
        vec4 world = sceneInputs.inverseViewProjection * vec4(uv * 2.0 - 1.0, sceneRawDepth(uv) * 2.0 - 1.0, 1.0);
        return world.xyz / world.w;
    }
#endif

#ifdef SURFACE_SCENE_COLOR
    // linear colour of the opaque scene (and sky) at a screen UV, bilinearly filtered; UVs
    // outside 0..1 clamp to the edge
    vec3 sceneColor(vec2 uv){
        vec2 colorUV = vec2(uv.x, uv.y * sceneInputs.colorParams.x + sceneInputs.colorParams.y);
        return sRGBToLinear(textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), colorUV, 0.0).rgb);
    }
#endif

#endif // SURFACE_SCENE_DEPTH || SURFACE_SCENE_COLOR

#endif // SCENE_GLSL
