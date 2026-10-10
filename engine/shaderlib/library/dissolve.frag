#version 450

// @shader mesh
// @name "Dissolve"
// @category "Effects"
// @description "Burns the surface away where a noise texture is below the cutoff, with a glowing edge band; both faces drawn, so the inside shows through the holes. Shadows get the same holes."
// @lighting none pixel
// @require uv1 color normals
// @state blend=opaque cull=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property cutoff range(0, 1) 0.35 "Cutoff (0 whole, 1 gone)"
// @property edgeWidth range(0, 0.5) 0.08 "Edge width (noise units)"
// @property edgeColor hdrcolor (2.5, 1.2, 0.35, 1) "Edge colour"
// @property noiseTiling vector4 (1, 1, 0, 0) "Noise tiling (xy) and offset (zw)"
// @property worldNoiseScale float 0.6 "Noise tiles per world unit" feature=WORLD_NOISE
// @texture u_baseTexture white "Base map"
// @texture u_noiseTexture gray "Noise (red)"
// @feature WORLD_NOISE "World-space noise"
// @feature VERTEX_COLOR "Vertex colour"
// @depth

// A fragment whose noise is below the clip level (library/dissolve.glsl) is dropped, here
// and in the depth program; above it, an edge band of edgeWidth turns to edgeColor (not
// lit: it glows). WORLD_NOISE samples the noise triplanar in world space instead of on
// the UVs, so it crosses UV seams without a cut (and stays put while the object moves).
// Pixel lighting is Lambert from the ambient and the pixel lights, with shadows, each face
// from its own side. Light units: includes/lights.glsl.

#include "library/dissolve.glsl"

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

#ifdef USE_PUNCTUAL
    in vec3 v_position;
    in vec3 v_normal;

    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    float noise = dissolveNoise(v_uv1);
    if (noise < dissolveClipLevel()){
        discard;
    }

    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    vec3 color = base.rgb;

    #ifdef USE_PUNCTUAL
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing){
            n = -n;
        }
        vec3 light = ambientLight(n);
        for (int i = 0; i < LIGHT_CAPACITY; i++){
            if (i >= pixelLightCount()) break;
            LightSample lightSample = getLight(i, v_position);
            float shadow = 1.0;
            #ifdef USE_SHADOWS
                shadow = lightShadow(i, v_position, n);
            #endif
            light += lambert(lightSample, n) * shadow;
        }
        color *= light;
    #endif

    float edge = dissolveEdge(noise);
    color = mix(color, customParams.edgeColor.rgb, edge * edge * (3.0 - 2.0 * edge));

    g_finalColor = finalizeColor(vec4(color, base.a));
}
