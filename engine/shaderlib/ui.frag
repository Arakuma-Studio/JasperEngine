#version 450

// A fork may declare a u_fs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Two names are
// written by the engine instead: "time" (seconds since startup) and "resolution"
// (xy = render target size, zw = 1 / size). Do not mix int and float members: GL
// uploads the block typed after its first member.
//
//   uniform u_fs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;

uniform u_fs_uiParams {
    vec4 color;
} uiParams;

out vec4 g_finalColor;

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 v_color;
#endif

#if defined(HAS_TEXTURE) || defined(HAS_FONTATLAS_TEXTURE)
    uniform texture2D u_uiTexture;
    uniform sampler u_ui_smp;
    in vec2 v_uv1;
#endif

#ifdef HAS_FONTATLAS_SDF
    // a Text with outline or shadow: its atlas holds signed distances (stb: 128 on the edge)
    uniform u_fs_textParams {
        vec4 outlineColor;  // linear, alpha
        vec4 shadowColor;   // linear, alpha
        vec4 params;        // x outline width, y shadow softness, z shadow dilate (atlas pixels), w distance scale
        vec4 shadowUV;      // xy: the shadow's offset in atlas uv
    } textParams;
#endif

#include "includes/srgb.glsl"

vec4 getVertexColor(){
   vec4 color = vec4(1.0, 1.0, 1.0, 1.0);

    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb = v_color.rgb;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC4
        color = v_color;
    #endif

   return color;
}

vec4 getBaseColor(){
    vec4 baseColor = uiParams.color;
    #ifdef HAS_TEXTURE
        baseColor *= sRGBToLinear(texture(sampler2D(u_uiTexture, u_ui_smp), v_uv1));
    #endif
    #ifdef HAS_FONTATLAS_TEXTURE
        baseColor *= vec4(1.0, 1.0, 1.0, texture(sampler2D(u_uiTexture, u_ui_smp), v_uv1).r);
    #endif
    return baseColor * getVertexColor();
}

#ifdef HAS_FONTATLAS_SDF
// atlas pixels from the glyph edge, positive inside
float sdfDistance(vec2 uv){
    return (texture(sampler2D(u_uiTexture, u_ui_smp), uv).r - 128.0 / 255.0) * textParams.params.w;
}

// face, outline and shadow (TMP's underlay) in one pass, antialiased over a screen pixel
vec4 getSdfColor(){
    vec4 vertexColor = getVertexColor();
    vec4 face = uiParams.color * vertexColor;
    // a glyph's fade (Text Effect, typewriter) and the Text's alpha fade its outline and shadow too
    float fade = vertexColor.a * uiParams.color.a;

    float dist = sdfDistance(v_uv1);
    float aa = max(fwidth(dist), 0.0001);
    float faceAlpha = clamp(dist / aa + 0.5, 0.0, 1.0);
    float outlineAlpha = clamp((dist + textParams.params.x) / aa + 0.5, 0.0, 1.0);

    // premultiplied while compositing
    vec4 color = vec4(face.rgb * face.a, face.a) * faceAlpha;
    float outline = textParams.outlineColor.a * fade * max(outlineAlpha - faceAlpha, 0.0);
    color += vec4(textParams.outlineColor.rgb * outline, outline);

    if (textParams.shadowColor.a > 0.0){
        float shadowDist = sdfDistance(v_uv1 - textParams.shadowUV.xy) + textParams.params.z;
        float band = textParams.params.y + 0.5 * aa;
        float shadowAlpha = smoothstep(-band, band, shadowDist) * textParams.shadowColor.a * fade;
        color += vec4(textParams.shadowColor.rgb * shadowAlpha, shadowAlpha) * (1.0 - color.a);
    }

    return (color.a > 0.0) ? vec4(color.rgb / color.a, color.a) : vec4(0.0);
}
#endif

void main() {  
    #ifdef HAS_FONTATLAS_SDF
        vec4 color = getSdfColor();
    #else
        vec4 color = getBaseColor();
    #endif

    g_finalColor = (vec4(linearTosRGB(color.rgb), color.a));
}