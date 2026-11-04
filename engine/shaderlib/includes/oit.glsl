// Weighted blended order-independent transparency (USE_OIT, docs/specs/S57-forge-lighting.md R4;
// McGuire and Bavoil, JCGT 2013), in the single-blend-state form GL and GLES3 allow (sokol has no
// per-attachment blend there): both targets blend RGB with ONE, ONE and alpha with ZERO,
// ONE_MINUS_SRC_ALPHA, so
//   target 0 (RGBA16F, cleared to 0, 0, 0, 1): rgb sums colour * alpha * weight, alpha keeps the
//            product of (1 - alpha), the revealage
//   target 1 (R16F, cleared to 0): r sums alpha * weight
// and oit_resolve.frag lays their weighted average over the scene. The colour is the one the
// sorted path blends (display-referred, straight alpha), so a single layer looks the same.
//
// The shader compiler wraps the fragment shader of an OIT variant (ShaderVariantCompiler): its
// main runs first, then oitAccumulate turns its straight-alpha g_finalColor into the outputs.

#ifndef OIT_GLSL
#define OIT_GLSL

layout(location = 1) out vec4 g_oitWeight;

uniform u_fs_oit {
    vec4 params;   // x the camera's weight scale: the view depth the weight takes is distance * scale
} oit;

// McGuire and Bavoil's depth weight (their equation 7, alpha apart), at a view depth in metres
float oitDepthWeight(float viewDepth){
    float z = abs(viewDepth) * oit.params.x;
    return clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e3);
}

// the accumulation of a straight-alpha colour; target 1 is written here, target 0 is returned
vec4 oitAccumulate(vec4 color){
    float alpha = clamp(color.a, 0.0, 1.0);
    // 1 / gl_FragCoord.w is the clip w: the view depth of a perspective camera, 1 for an
    // orthographic one (whose fragments then weigh the same)
    float weight = alpha * oitDepthWeight(1.0 / gl_FragCoord.w);
    g_oitWeight = vec4(alpha * weight);
    return vec4(color.rgb * alpha * weight, alpha);
}

#endif // OIT_GLSL
