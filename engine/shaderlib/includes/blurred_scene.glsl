// The blurred scene of the frame (shader library phase 4): the blur chain's image of the
// scene below (PostProcessingSettings::blurEnabled; blurIntensity 0 makes only this texture,
// not a blurred screen), for frosted UI panels and other "glass over the game" effects.
// Fragment stage, any shader type.
//
// It exists for the draws after the uber pass of a base-layer scene with Blur on, in the same
// view: a layer scene's (Engine.addSceneLayer: a HUD drawn over the post-processed scene).
// Anywhere else (the scene with the blur itself, whose image the blur is made from; a frame
// without blur) it is black and blurredSceneAvailable() is 0, so a shader can fall back.
//
// The engine binds, by name: the uniform block u_fs_blurredScene and u_blurredSceneTexture /
// u_blurredScene_smp (SRGB8A8 at a quarter of the scene's resolution, linear filtering).

#ifndef BLURRED_SCENE_GLSL
#define BLURRED_SCENE_GLSL

#include "includes/srgb.glsl"

// engine-written per draw (blurred_scene_t in RenderSystem.h)
uniform u_fs_blurredScene {
    vec4 screenToUV; // blurred texture UV = gl_FragCoord.xy * xy + zw
    vec4 params;     // x 1 when the blurred scene is bound, 0 when there is none
} blurredSceneParams;

uniform texture2D u_blurredSceneTexture;
uniform sampler u_blurredScene_smp;

// the blurred scene's UV under the current fragment
vec2 blurredSceneUV(){
    return gl_FragCoord.xy * blurredSceneParams.screenToUV.xy + blurredSceneParams.screenToUV.zw;
}

// 1 when the blurred scene is there, 0 when the texture is black for want of one
float blurredSceneAvailable(){
    return blurredSceneParams.params.x;
}

// linear colour of the blurred scene at a UV (the texture holds the gamma-2 decode of the
// display colour, which this turns into the engine's linear colour: finalizeColor encodes it
// back to the scene's display colour exactly)
vec3 blurredScene(vec2 uv){
    return sRGBToLinear(sqrt(textureLod(sampler2D(u_blurredSceneTexture, u_blurredScene_smp), uv, 0.0).rgb));
}

#endif // BLURRED_SCENE_GLSL
