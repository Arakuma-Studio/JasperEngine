#version 450

// @shader mesh
// @name "Projected decal"
// @category "Effects"
// @description "Box projector: put it on a unit cube (1 x 1 x 1) and scale, turn and move the cube over uneven ground, stairs or a wall; the texture projects down the box's Y axis onto whatever opaque surface is inside the box, fading on steep faces and towards the box's top and bottom. Alpha blended (the Multiply default blend darkens instead), unlit, no shadows; works with the camera inside the box. Give its material a sort priority below the transparent objects it may cover (-1). Reads the scene depth: on mobile that is an extra depth pass of the opaque meshes in every camera that sees it (none when SSAO or SSR already made one)."
// @require color
// @require scenedepth
// @state blend=alpha zwrite=off ztest=always cull=front shadows=off
// @state[DECAL_BLEND_MULTIPLY] blend=multiply
// @property tint color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property opacity range(0, 1) 1 "Opacity"
// @property edgeFade range(0.001, 0.5) 0.1 "Fade towards the box's top and bottom (fraction of its height)"
// @property angleFadeStart range(0, 90) 50 "Angle fade start (degrees from the box's up axis)" feature=ANGLE_FADE
// @property angleFadeEnd range(0, 90) 70 "Angle fade end (degrees: gone from here)" feature=ANGLE_FADE
// @texture u_baseTexture white "Decal"
// @feature ANGLE_FADE "Fade on steep surfaces" default=on
// @keywords DECAL_BLEND "Default blend" ALPHA MULTIPLY

// The box's back faces (cull=front) cover its whole screen area, from outside and from
// inside, and draw whatever is in front of them (ztest=always: the surfaces the decal lands
// on hide those faces). Each pixel reconstructs the world position of the opaque surface
// under it (sceneWorldPosition), moves it into the box's space with the inverse model
// matrix the vertex stage passes (library/projected_decal.vert) and is discarded outside the
// unit box (-0.5 .. 0.5), so geometry in front of or behind the box, and the sky, are
// untouched. The texture maps the box's XZ like a plane of the box's size seen from above
// (u along +X, v along +Z, tiling and offset on top): it projects straight down the box's Y
// axis, so on flat ground it is not stretched, whatever the view.
//
// ANGLE_FADE (default on) fades it out where the surface turns away from the box's up axis
// (from angleFadeStart to angleFadeEnd degrees: stair risers, the sides of rocks, a wall
// inside a ground decal's box), using the surface normal reconstructed from the screen
// derivatives of the reconstructed position: no extra depth samples, at the price of a
// pixel-wide seam (the normal mixes two surfaces) along depth steps such as a stair nosing.
// The cheaper fixed alternative (fade by the box's height alone) cannot tell a riser from a
// tread, so it would stretch the texture down every step. Turn it off for a decal that
// should wrap everything (a wall decal: rotate the box so its Y axis points out of the wall).
// edgeFade fades it towards the box's top and bottom faces, so a slope leaving the box does
// not end in a hard line.
//
// The fog is the surface's: the decal's colour is fogged at the fog distance the surface
// under it got (sceneFogDistance, includes/output.glsl), not at the box's back face; the
// multiply blend fades to white by it. The texture's mip level comes from the screen
// derivatives of its UV, which jump where the surface under neighbouring pixels jumps in
// depth (a stair nosing, an object's outline): each derivative is held to 4 times the
// smaller one, so those pixels do not take a tiny mip (a blurred outline) while a surface
// seen at a grazing angle keeps up to 4:1 of its stretch. A box scaled to zero along an axis
// (no inverse) draws nothing.
//
// The blend follows the variant (finalizeColor; colour as the texture stores it): alpha
// blends texture * tint * instance colour by its alpha times opacity and the fades; the
// Multiply default blend (DECAL_BLEND_MULTIPLY, @state[...]) darkens what is under it by that
// colour faded to white. Unlit: an alpha decal does not darken in shadow, a multiply one
// keeps the surface's lighting. It draws in the transparent queue: give its material a
// sort priority below the transparent objects it may cover (-1), so it draws before them
// (with ztest=always a decal drawn later would paint over them). Moving objects inside the
// box take the decal too (they are in the scene depth); transparent ones never do.

uniform u_fs_customParams {
    vec4 tint;
    vec4 tiling;
    float opacity;
    float edgeFade;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

flat in vec4 v_decalRow0;
flat in vec4 v_decalRow1;
flat in vec4 v_decalRow2;
in vec4 v_color; // the instance colour

#ifdef ANGLE_FADE
    #define FRAME_FS
    #include "includes/frame.glsl"
    // cos of angleFadeStart and angleFadeEnd (projected_decal.vert)
    flat in vec2 v_angleFade;
#endif

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    // the opaque surface under the pixel (sceneWorldPosition, keeping its stored depth for
    // the fog)
    vec2 screen = screenUV();
    float depth01 = sceneRawDepth(screen);
    vec4 world4 = sceneInputs.inverseViewProjection * vec4(screen * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    vec3 world = world4.xyz / world4.w;
    vec4 p = vec4(world, 1.0);
    vec3 local = vec3(dot(v_decalRow0, p), dot(v_decalRow1, p), dot(v_decalRow2, p));

    // before any discard: derivatives need the whole 2x2 quad
    #ifdef ANGLE_FADE
        vec3 normal = cross(dFdx(world), dFdy(world));
    #endif
    vec2 uv = (local.xz + 0.5) * customParams.tiling.xy + customParams.tiling.zw;
    // the UV's derivatives, each held to 4 times the smaller (a depth step under the quad
    // makes one jump)
    vec2 uvdx = dFdx(uv);
    vec2 uvdy = dFdy(uv);
    float limit = 16.0 * min(dot(uvdx, uvdx), dot(uvdy, uvdy));
    uvdx *= sqrt(min(limit / max(dot(uvdx, uvdx), 1e-20), 1.0));
    uvdy *= sqrt(min(limit / max(dot(uvdy, uvdy), 1e-20), 1.0));
    vec4 color = sRGBToLinear(textureGrad(sampler2D(u_baseTexture, u_base_smp), uv, uvdx, uvdy));

    // outside the unit box, or not a number (a box scaled to zero: the vertex stage sends it
    // outside too)
    vec3 outside = abs(local);
    if (!(max(outside.x, max(outside.y, outside.z)) <= 0.5)){
        discard;
    }

    float fade = customParams.opacity * clamp((0.5 - outside.y) / customParams.edgeFade, 0.0, 1.0);

    #ifdef ANGLE_FADE
        // face the camera (the derivatives' handedness differs between backends); the box's
        // up axis in world space is the gradient of its local y
        vec3 toCamera = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : frame.cameraPosition.xyz - world;
        normal *= sign(dot(normal, toCamera));
        float facing = dot(normal, normalize(v_decalRow1.xyz)) * inversesqrt(max(dot(normal, normal), 1e-20));
        fade *= clamp((facing - v_angleFade.y) / max(v_angleFade.x - v_angleFade.y, 0.0001), 0.0, 1.0);
    #endif

    color *= customParams.tint * v_color;
    // the colour as the texture stores it: premultiplied for that blend, so fade it all
    #ifdef BLEND_PREMULTIPLIED
        color *= fade;
    #else
        color.a *= fade;
    #endif

    // fogged as the surface under it was
    g_finalColor = finalizeColorAtFogDistance(color, sceneFogDistance(screen, depth01));
}
