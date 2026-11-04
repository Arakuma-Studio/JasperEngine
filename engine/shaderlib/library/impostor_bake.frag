#version 450

// @shader mesh
// @name "Impostor Bake"
// @category "Internal"
// @description "What Bake Impostor renders a tree with: its base colour unlit, or with IMPOSTOR_NORMAL its normal in the view's frame, with the cutout's alpha in both."
// @lighting none
// @require uv1 normals
// @state blend=opaque cull=off
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff"
// @property viewRight vector4 (1, 0, 0, 0) "The view's right, world"
// @property viewUp vector4 (0, 1, 0, 0) "The view's up, world"
// @property viewForward vector4 (0, 0, 1, 0) "Towards the camera, world"
// @texture u_baseTexture white "Base map (alpha: cutout)"
// @feature IMPOSTOR_NORMAL "Write the normal"

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 viewRight;
    vec4 viewUp;
    vec4 viewForward;
    float alphaCutoff;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec3 v_normal;

out vec4 g_finalColor;

void main() {
    vec4 base = customParams.baseColor * texture(sampler2D(u_baseTexture, u_base_smp), v_uv1);
    if (base.a < customParams.alphaCutoff) {
        discard;
    }
    #ifdef IMPOSTOR_NORMAL
        vec3 n = normalize(v_normal);
        // each face is seen from its own side
        if (!gl_FrontFacing) n = -n;
        vec3 stored = vec3(dot(n, customParams.viewRight.xyz), dot(n, customParams.viewUp.xyz), dot(n, customParams.viewForward.xyz));
        // alpha: baked ambient occlusion's place; the bake takes coverage from the albedo pass
        g_finalColor = vec4(stored * 0.5 + 0.5, 1.0);
    #else
        g_finalColor = vec4(base.rgb, 1.0);
    #endif
}
