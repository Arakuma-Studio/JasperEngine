#version 450

// @shader mesh
// @name "Depth fade"
// @category "Effects"
// @description "Fog cards, light shafts and soft volumes: texture times tint and vertex colour, fading where it meets geometry (no hard line where a card crosses the ground or a wall), close to the camera and far away, optionally scrolling, masked and faded when seen edge-on. Alpha blended (the Additive default blend for light shafts), both faces, no depth write or shadows. The soft intersections read the scene depth: on mobile that is an extra depth pass of the opaque meshes in every camera that sees it (none when SSAO or SSR already made one); turn Soft off for camera fades alone."
// @require uv1 color
// @require scenedepth feature=SOFT
// @require normals position feature=EDGE_FADE
// @state blend=alpha zwrite=off cull=off shadows=off
// @state[FADE_BLEND_ADDITIVE] blend=additive
// @property tint hdrcolor (1, 1, 1, 0.6) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property softness float 1 "Softness (world units)" feature=SOFT
// @property nearFadeStart float 0.5 "Near fade: invisible at (world units from the camera)"
// @property nearFadeEnd float 3 "Near fade: whole from (world units from the camera)"
// @property farFadeStart float 60 "Far fade: starts at (world units from the camera)"
// @property farFadeEnd float 80 "Far fade: invisible from (world units from the camera)"
// @property scrollSpeed vector2 (0.03, 0.01) "Scroll (UV per second)" feature=SCROLL
// @property edgeFadePower range(0.25, 8) 1.5 "Edge-on fade power" feature=EDGE_FADE
// @texture u_baseTexture white "Base map (fog, beam)"
// @texture u_maskTexture white "Shape mask (red times alpha, not tiled or scrolled)" feature=MASK
// @feature SOFT "Soft intersections (reads the scene depth)" default=on
// @feature SCROLL "Scrolling base map"
// @feature MASK "Shape mask"
// @feature EDGE_FADE "Fade when seen edge-on"
// @keywords FADE_BLEND "Default blend" ALPHA ADDITIVE

// colour = tint * base map (tiled, scrolled with SCROLL) * vertex and instance colour, its
// alpha times:
// - SOFT (default on): saturate((scene depth - fragment depth) / softness), the soft
//   intersection: no hard line where the card crosses the ground, a wall or a rock;
// - the camera fades, by the view-space distance (world units along the camera's axis
//   from the camera plane, the same in every backend and in orthographic cameras): from
//   invisible at nearFadeStart to whole at nearFadeEnd (a card the camera flies through
//   melts away before the near plane cuts it), and from whole at farFadeStart to invisible
//   at farFadeEnd (cards that thin out with distance instead of piling up). This replaces a
//   fade on the clip-space z / w, whose range differs between platforms;
// - MASK: a shape mask at the mesh's own UVs (not tiled or scrolled), to shape a card whose
//   base map tiles and scrolls: its red times its alpha, so a grey mask (opaque) and a white
//   sprite with a soft alpha (a particle texture) both work;
// - EDGE_FADE: pow(|n.v|, edgeFadePower), so a card seen edge-on fades instead of showing
//   it is flat (light shafts, crossed cards); both faces fade alike.
// Straight alpha (finalizeStraightColor): the premultiplied blend fades the colour too, the
// additive one adds less, the multiply one fades to white. library/soft_fade.glsl has the
// fades' maths.

uniform u_fs_customParams {
    vec4 tint;
    vec4 tiling;
    #ifdef SCROLL
        vec2 scrollSpeed;
    #endif
    float nearFadeStart;
    float nearFadeEnd;
    float farFadeStart;
    float farFadeEnd;
    #ifdef SOFT
        float softness;
    #endif
    #ifdef EDGE_FADE
        float edgeFadePower;
    #endif
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#ifdef MASK
    uniform texture2D u_maskTexture;
    uniform sampler u_mask_smp;
#endif

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the vertex colour times the instance colour

#ifdef EDGE_FADE
    in vec3 v_position;
    in vec3 v_normal;
#endif

out vec4 g_finalColor;

// the frame block for the scroll time, the edge-on view and the camera fades without SOFT
#if defined(SCROLL) || defined(EDGE_FADE)
    #define FRAME_FS
    #include "includes/frame.glsl"
#endif
#include "library/soft_fade.glsl"
#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    #ifdef SCROLL
        // wraps every UV unit, so a long run keeps its precision
        uv += fract(frame.time.x * customParams.scrollSpeed);
    #endif
    vec4 color = customParams.tint * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;

    float viewDistance = softViewDistance();
    float fade = softRamp(viewDistance, customParams.nearFadeStart, customParams.nearFadeEnd) *
                 softRamp(viewDistance, customParams.farFadeEnd, customParams.farFadeStart);
    #ifdef SOFT
        fade *= softIntersection(customParams.softness);
    #endif
    #ifdef MASK
        vec4 mask = texture(sampler2D(u_maskTexture, u_mask_smp), v_uv1);
        fade *= mask.r * mask.a;
    #endif
    #ifdef EDGE_FADE
        vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
        // min: |n.v| of two unit vectors can round above 1
        fade *= pow(min(abs(dot(normalize(v_normal), v)), 1.0), customParams.edgeFadePower);
    #endif
    color.a *= fade;

    g_finalColor = finalizeStraightColor(color);
}
