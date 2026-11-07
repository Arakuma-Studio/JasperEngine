#version 450

// @shader postprocess
// @name "Underwater"
// @category "Post-processing"
// @role underwater
// @description "The view from inside a water: the scene fades into the water with the distance the view travels through it, each colour at its own rate (Beer-Lambert: a blue-green water loses red first), towards the water's colour lit by the sun and the ambient; the view leaving the water at its surface fades only by the water in front of it. Add it to the chain switched off: while the main camera is under a library:water surface (a running scene) the engine switches it on and gives it that surface's height and, with Follow water, its shallow and deep colours, and switches it off again above (Water, engine/core/object/environment/Water.h). Reads the scene depth: a depth pre-pass of the opaque meshes while it is on (none extra with SSAO or SSR); one full-screen pass."
// @property shallowColor color (0.25, 0.8, 0.85, 1) "Clear colour (the hue the view fades through)"
// @property deepColor color (0.03, 0.25, 0.45, 1) "Water colour (what the distance fades to)"
// @property visibility float 6 "Visibility (world units: the clearest colour keeps 37%)"
// @property waterLevel float 0 "Surface height (world Y; the engine sets it under a water)"
// @property followWater toggle 1 "Follow water (colours from the water the camera is in)"

// For each pixel the view ray (the frame block's camera; orthographic too) from the camera to
// the opaque scene (the scene depth, u_depthTexture; the far plane where there is none) is cut
// to its part below waterLevel, the path through the water. Transmittance per channel:
// exp(-k * path / visibility), k = log(c) / log(max c) of the clear colour c (1 for its
// clearest channel), so the clearest one keeps exp(-1) after visibility units. The scene
// colour (display-encoded, decoded) times the transmittance, plus the water's colour
// (deepColor times the light reaching it: the frame's ambient and the main directional light
// by its height, Lambert's 1/PI as includes/lights.glsl) times what it took away. The water
// surface (library:water, UNDERSIDE: Snell's window) and whatever is above it are past
// waterLevel: they fade by the water in front of them only. waterLevel and followWater are
// read by the engine: under a water it writes waterLevel (the surface at the camera, waves
// included) and, with followWater on, shallowColor and deepColor (the water material's); off,
// the pass keeps its own colours. Display colour in and out.

#define FRAME_FS
#include "includes/frame.glsl"
#include "includes/srgb.glsl"
#include "includes/depth_util.glsl"

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_postParams {
    vec4 shallowColor;
    vec4 deepColor;
    float visibility;
    float waterLevel;
    float followWater;
} postParams;

const float UNDERWATER_INV_PI = 0.31830988618;

void main(){
    // the scene targets keep the top of the view in row 0; the depth keeps the pre-pass's
    // orientation (depth_util.glsl)
    vec2 ndc = vec2(v_texcoord.x * 2.0 - 1.0, 1.0 - v_texcoord.y * 2.0);
    float depth01 = min(decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), vec2(v_texcoord.x, ndcYToDepthUV(ndc.y)), 0.0)), 1.0);
    float zNdc = depth01 * 2.0 - 1.0;

    // the view ray of the pixel in view space (forward component 1; |P11|: a flipped
    // render-to-texture projection keeps the logical y), and the distance to the scene
    mat4 projection = frame.projectionMatrix;
    bool orthographic = frame.cameraPosition.w > 0.5;
    vec2 ndcToView = vec2(1.0 / projection[0][0], 1.0 / abs(projection[1][1]));
    vec3 viewRay;
    vec3 viewOrigin = vec3(0.0);
    float forward;
    if (orthographic){
        viewRay = vec3(0.0, 0.0, -1.0);
        viewOrigin = vec3(ndc * ndcToView, 0.0);
        forward = (projection[3][2] - zNdc) / projection[2][2];
    }else{
        viewRay = vec3(ndc * ndcToView, -1.0);
        forward = projection[3][2] / (zNdc + projection[2][2]);
    }
    float sceneDistance = max(forward, 0.0) * length(viewRay);

    // to world space (the view matrix's rotation is orthonormal: its transpose inverts it)
    mat3 viewRotation = mat3(frame.viewMatrix);
    vec3 direction = normalize(viewRay * viewRotation);
    float originY = frame.cameraPosition.y + (viewOrigin * viewRotation).y;

    // the part of the ray under the surface
    float level = postParams.waterLevel;
    float path;
    if (originY < level){
        float exit = direction.y > 1e-5 ? (level - originY) / direction.y : sceneDistance;
        path = min(sceneDistance, exit);
    }else{
        float entry = direction.y < -1e-5 ? (level - originY) / direction.y : sceneDistance;
        path = max(sceneDistance - entry, 0.0);
    }

    vec3 tint = clamp(postParams.shallowColor.rgb, vec3(0.001), vec3(0.999));
    vec3 extinction = log(tint) / log(max(tint.r, max(tint.g, tint.b)));
    vec3 transmittance = exp(-extinction * (path / max(postParams.visibility, 1e-3)));

    // the water lit by the ambient and the sun reaching it from above
    vec3 sunlight = frame.mainLightColor.rgb * (frame.mainLightDirection.w * max(frame.mainLightDirection.y, 0.0) * UNDERWATER_INV_PI);
    vec3 scattered = postParams.deepColor.rgb * (frame.ambient.rgb + sunlight);

    vec3 scene = sRGBToLinear(texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb);
    frag_color = vec4(linearTosRGB(scene * transmittance + scattered * (vec3(1.0) - transmittance)), 1.0);
}
