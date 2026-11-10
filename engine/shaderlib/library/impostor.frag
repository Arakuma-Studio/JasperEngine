#version 450

// @shader mesh
// @name "Tree Impostor"
// @category "Nature"
// @description "The last LOD of a tree prototype: one quad per instance, facing the camera around Y, showing the baked view nearest the camera and lit through the baked normals. Baked by Bake Impostor in Paint Trees or edit_terrain's bake_impostors."
// @lighting pixel vertex none
// @require uv1
// @require color
// @state blend=opaque cull=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff"
// @texture u_baseTexture white "Albedo atlas (alpha: cutout)"
// @texture u_normalTexture normal "Normal atlas (a: ambient occlusion)"
// @depth

// F7: two views blend on Medium and High (four fragment textures, u_vs_foliage.heightRange.z), the
// nearest on Low (two). Normals are stored in the view's frame: x right, y up, z towards the camera.

uniform u_fs_customParams {
    vec4 baseColor;
    float alphaCutoff;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;
uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color;
in vec2 v_impostorUv0;
in vec2 v_impostorUv1;
in float v_impostorBlend;
in vec3 v_impostorRight;
in vec3 v_impostorForward;

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

#include "includes/foliage_dither.glsl"

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    #ifdef USE_FOLIAGE_DITHER
        foliageDitherDiscard();
    #endif

    vec4 albedo = sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), v_impostorUv0));
    vec4 packedNormal = texture(sampler2D(u_normalTexture, u_normal_smp), v_impostorUv0);
    if (v_impostorBlend > 0.0) {
        albedo = mix(albedo, sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), v_impostorUv1)), v_impostorBlend);
        packedNormal = mix(packedNormal, texture(sampler2D(u_normalTexture, u_normal_smp), v_impostorUv1), v_impostorBlend);
    }

    vec4 base = customParams.baseColor * albedo * v_color;
    if (base.a < customParams.alphaCutoff) {
        discard;
    }

    vec3 color = base.rgb;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 light = vec3(0.0);
        #ifdef USE_PUNCTUAL
            vec3 stored = packedNormal.xyz * 2.0 - 1.0;
            // the view's frame is terrain-local; a Terrain's rotation is about Y in every use
            vec3 n = normalize(v_impostorRight * stored.x + vec3(0.0, 1.0, 0.0) * stored.y + v_impostorForward * stored.z);
            light += ambientLight(n) * packedNormal.a;
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample lightSample = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                light += lambert(lightSample, n) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            light += v_vertexLight;
        #endif
        color *= light;
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
