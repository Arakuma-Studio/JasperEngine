#version 450

// @shader mesh
// @name "Foliage"
// @category "Nature"
// @description "Leaves and grass: wind sway in the vertex stage (masked by the vertex colour's red or by the height), alpha cutout, both faces drawn, light through back-lit leaves; the instance colour tints, the vertex colour does not. Shadows sway and cut out with it."
// @lighting pixel vertex none
// @require uv1 color
// @state blend=opaque cull=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff" feature=ALPHA_CLIP
// @property translucencyColor color (0.75, 0.9, 0.3, 1) "Translucency colour (per pixel lights)"
// @property translucency range(0, 2) 0.8 "Translucency (per pixel lights)"
// @property windDirection vector3 (1, 0, 0.35) "Wind direction (world)"
// @property windStrength float 0.08 "Sway (world units)"
// @property windSpeed float 0.7 "Sway speed (cycles per second)"
// @property windScale float 0.25 "Wave scale (waves per world unit downwind)"
// @property gustStrength range(0, 2) 0.6 "Gust strength"
// @property gustFrequency float 0.15 "Gust frequency (cycles per second)"
// @property flutter range(0, 1) 0.2 "Leaf flutter"
// @property heightStart float 0 "Height where sway starts (object units)" feature=WIND_MASK_HEIGHT
// @property heightRange float 1 "Height to full sway (object units)" feature=WIND_MASK_HEIGHT
// @texture u_baseTexture white "Base map (alpha: cutout)"
// @feature ALPHA_CLIP "Alpha cutout" default=on
// @keywords WIND_MASK "Wind mask" VERTEX_COLOR HEIGHT
// @depth

// The wind (library/foliage.glsl) moves the plant as a whole, its phase from where it
// stands so a field ripples downwind, with slow gusts and a little per-vertex flutter.
// WIND_MASK_VERTEX_COLOR: the vertex colour's red is how much a vertex sways (0 at the
// roots; the raw vertex colour, so an instance colour tints the leaves without changing
// the sway); WIND_MASK_HEIGHT: the sway grows from
// heightStart to heightStart + heightRange above the origin. Both faces are lit from
// their own side; per pixel lights also shine through a leaf seen against them
// (translucency). Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec3 translucencyColor;
    float alphaCutoff;
    float translucency;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the instance colour (the vertex colour never tints, foliage.vert)

#ifdef USE_PUNCTUAL
    in vec3 v_position;
    in vec3 v_normal;

    #define FRAME_FS
    #include "includes/frame.glsl"
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

#include "includes/foliage_dither.glsl"

void main() {
    #ifdef USE_FOLIAGE_DITHER
        foliageDitherDiscard();
    #endif
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;

    #ifdef ALPHA_CLIP
        // the same test as foliage_depth.frag
        if (base.a < customParams.alphaCutoff){
            discard;
        }
    #endif

    vec3 color = base.rgb;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 light = vec3(0.0);
        #ifdef USE_PUNCTUAL
            vec3 n = normalize(v_normal);
            // each face is lit from its own side
            if (!gl_FrontFacing){
                n = -n;
            }
            // towards the camera (an orthographic camera looks along one direction)
            vec3 v = (frame.cameraPosition.w > 0.5) ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
            vec3 translucent = customParams.translucencyColor * customParams.translucency;

            light += ambientLight(n);
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample lightSample = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                // light reaching the hidden face comes through, strongest looking into the light
                float through = 0.5 + 2.0 * pow(max(dot(v, -lightSample.direction), 0.0), 4.0);
                light += (lambert(lightSample, n) + lambert(lightSample, -n) * through * translucent) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            light += v_vertexLight;
        #endif
        color *= light;
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
