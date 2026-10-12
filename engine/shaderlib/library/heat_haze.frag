#version 450

// @shader mesh
// @name "Heat Haze"
// @category "Effects"
// @description "Shimmer over fire, exhausts and portals: the scene behind the mesh wobbled by two layers of a scrolling noise texture, as strong as the mask texture, the vertex and instance alpha and the soft edge (UV border for quads and sprites, fresnel for volumes) allow. Reads the scene colour: on mobile a copy of the screen (half resolution by default) and a split of the render pass; SOFT also reads the scene depth (an extra depth-only pass of the opaque meshes, none when SSAO or SSR already made one). Without SOFT, a shift that lands on an object in front of the haze pulls that object's colour into it (a smear along its outline): turn SOFT on where objects pass in front. Transparent objects behind the haze are hidden where it is at full strength (it shows the scene colour, which holds only opaque objects and the sky), faintly seen where it fades. The UV soft edge assumes 0..1 UVs (an atlas sprite fades at its rect's place in the atlas; use FRESNEL or NONE there). Alpha blended, no depth write, both faces (give a closed volume cull back), casts no shadows."
// @require uv1 color scenecolor
// @require normals position feature=EDGE_FADE_FRESNEL
// @require scenedepth feature=SOFT
// @state blend=alpha zwrite=off cull=off shadows=off
// @property strength range(0, 0.1) 0.012 "Distortion (screen UV)"
// @property noiseTiling vector4 (1, 1, 0, 0) "Noise tiling (xy) and offset (zw)"
// @property scrollSpeed vector2 (0, -0.6) "Noise scroll (UV per second)"
// @property secondScrollSpeed vector2 (0.07, -0.9) "Second noise layer scroll (UV per second, 1.7 times smaller)"
// @property tint color (1, 1, 1, 1) "Tint"
// @property edgeWidth range(0.01, 0.5) 0.25 "Soft edge width (UV)" feature=EDGE_FADE_UV
// @property fresnelPower range(0.25, 8) 1.5 "Edge fade power" feature=EDGE_FADE_FRESNEL
// @property softness float 0.4 "Soft intersection (world units)" feature=SOFT
// @texture u_noiseTexture gray "Distortion noise (red: x, green: y)"
// @texture u_maskTexture white "Mask (red times alpha)" feature=MASK_TEXTURE
// @keywords EDGE_FADE "Soft edge" UV FRESNEL NONE
// @feature MASK_TEXTURE "Mask texture"
// @feature SOFT "Soft intersection (reads the scene depth)"

// The noise's red and green, 0.5 meaning no shift, sampled twice (noiseTiling scrolled by
// scrollSpeed, and 1.7 times smaller scrolled by secondScrollSpeed), shift the scene colour
// sample by up to `strength` screen UV (x scaled by the aspect so the wobble is round). A
// gray noise (the default texture) shifts nothing: give it a smooth noise. The mask, the
// product of the mask texture's red times its alpha at uv1 (MASK_TEXTURE: a grey mask or a
// white texture shaped by its alpha), the vertex and instance alpha,
// the soft edge (EDGE_FADE: UV fades over edgeWidth from the UV border, for quads and
// sprites; FRESNEL by |n.v|^fresnelPower, for spheres, cones and other volumes; NONE) and
// SOFT, scales the shift and is the output's alpha, so the haze fades into the unchanged
// scene. SOFT also fades where the mesh comes within `softness` world units of the opaque
// scene behind it (no hard line where a quad meets the ground) and keeps the straight sample
// where the shifted one lands on something in front of the haze. The shift never reaches
// past the screen's border. Without SOFT nothing rejects a shifted sample landing on an
// object in front of the haze, which then smears that object's colour into it along its
// outline. tint multiplies the colour (a portal's glow); the scene colour has its fog
// already, so no fog is added, and the tint fades to white with the fog at the haze, as a
// multiply layer would. The output's alpha blends the shifted scene colour over what the
// target holds: where the mask is whole, transparent objects drawn before the haze are
// hidden (the scene colour holds the opaque scene and the sky only); where it fades they
// show through in proportion. The default noise scroll moves the pattern up a
// wall or a camera-facing quad (UV v down), as heat rises. Both faces are drawn so a quad
// shows from behind; a closed volume (a sphere portal) should cull its back faces, or its
// far side's UV seam shows through.

uniform u_fs_customParams {
    vec4 noiseTiling;
    vec4 tint;
    vec2 scrollSpeed;
    vec2 secondScrollSpeed;
    float strength;
    float edgeWidth;
    float fresnelPower;
    float softness;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;

#ifdef MASK_TEXTURE
    uniform texture2D u_maskTexture;
    uniform sampler u_mask_smp;
#endif

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // vertex x instance colour: its alpha masks the haze

#ifdef EDGE_FADE_FRESNEL
    in vec3 v_position;
    in vec3 v_normal;
#endif

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.noiseTiling.xy + customParams.noiseTiling.zw;
    // the scroll wraps every tile, so a long run keeps its precision
    vec2 noise = texture(sampler2D(u_noiseTexture, u_noise_smp), uv + fract(frame.time.x * customParams.scrollSpeed)).rg +
                 texture(sampler2D(u_noiseTexture, u_noise_smp), uv * 1.7 + fract(frame.time.x * customParams.secondScrollSpeed)).rg -
                 vec2(1.0);

    float mask = v_color.a;
    #ifdef MASK_TEXTURE
        vec4 maskTexel = texture(sampler2D(u_maskTexture, u_mask_smp), v_uv1);
        mask *= maskTexel.r * maskTexel.a;
    #endif
    #if defined(EDGE_FADE_UV)
        vec2 border = min(v_uv1, vec2(1.0) - v_uv1);
        mask *= smoothstep(0.0, customParams.edgeWidth, min(border.x, border.y));
    #elif defined(EDGE_FADE_FRESNEL)
        vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
        mask *= pow(clamp(abs(dot(normalize(v_normal), v)), 0.0, 1.0), customParams.fresnelPower);
    #endif

    vec2 screen = screenUV();
    #ifdef SOFT
        float surfaceDepth = fragmentLinearDepth();
        mask *= clamp((sceneLinearDepth(screen) - surfaceDepth) / max(customParams.softness, 1e-4), 0.0, 1.0);
    #endif

    vec2 shift = noise * (customParams.strength * mask);
    shift.x *= frame.resolution.y * frame.resolution.z;
    // never past the screen's border, where the copy would repeat its edge
    vec2 room = min(screen, vec2(1.0) - screen);
    shift *= clamp(min(room.x, room.y) / max(2.0 * length(shift), 1e-5), 0.0, 1.0);
    vec2 distorted = screen + shift;
    #ifdef SOFT
        if (sceneLinearDepth(distorted) < surfaceDepth){
            // something in front of the haze is not behind it
            distorted = screen;
        }
    #endif

    // the tint filters the fogged scene colour: the fog at the haze fades it to white
    vec3 color = sceneColor(distorted) * fadeByFog(customParams.tint.rgb, vec3(1.0));
    g_finalColor = finalizeFoggedColor(vec4(color, clamp(mask * customParams.tint.a, 0.0, 1.0)));
}
