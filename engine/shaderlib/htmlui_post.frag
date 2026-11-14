#version 450

// HTML UI fullscreen passes on premultiplied sRGB images, one exclusive variant each:
//   HAS_PASSTHROUGH    copy (layer composite, blits, stencil clears) times params.x (opacity); with
//                      params.w on, clipped to the rectangle texCoordMinMax (destination px of a
//                      viewport of params.yz px) by its coverage at the pixel's centre (S92: a
//                      layer composite under a rectangular clip, without the stencil)
//   HAS_COLOR_MATRIX   brightness/contrast/invert/grayscale/sepia/hue-rotate/saturate
//   HAS_BLEND_MASK     multiplies by the mask image's alpha (mask-image)
//   HAS_DROP_SHADOW    the source's alpha times the shadow colour, inside texCoordMinMax
//   (PASSTHROUGH and DROP_SHADOW, weights.w on: the mean of four bilinear taps weights.yz off the
//   texture coordinate, a 4 x 4 box: a blur's two halvings in one pass, S92)
//   HAS_BLUR           one 7-tap separable gaussian pass along params.yz, mirrored at texCoordMinMax
//   HAS_UNPREMULTIPLY  straight alpha (render-to-texture documents)
//   HAS_YUV            a video frame: Y (u_htmluiPostTex), U and V planes (R8) to opaque RGB by
//                      colorMatrix (offsets and range folded in its fourth column); params.yz
//                      scale the texture coordinates of the chroma planes (odd sizes)
uniform u_fs_htmluiPost {
    vec4 params;            // x: opacity (passthrough), yz: texel offset (blur)
    vec4 texCoordMinMax;    // xy: min, zw: max texture coordinate read (blur, drop shadow)
    vec4 color;             // drop shadow: premultiplied colour
    vec4 weights;           // blur: the gaussian weights of taps 0..3 (normalized)
    mat4 colorMatrix;       // colour matrix: rgb rows act on premultiplied rgba
} htmluiPost;

uniform texture2D u_htmluiPostTex;
uniform sampler u_htmluiPost_smp;
#ifdef HAS_BLEND_MASK
    uniform texture2D u_htmluiMaskTex;
    uniform sampler u_htmluiMask_smp;
#endif
#ifdef HAS_YUV
    uniform texture2D u_htmluiYuvU;
    uniform sampler u_htmluiYuvU_smp;
    uniform texture2D u_htmluiYuvV;
    uniform sampler u_htmluiYuvV_smp;
#endif

in vec2 v_uv;
in vec2 v_dst;

out vec4 g_finalColor;

vec4 source(vec2 uv){
    return texture(sampler2D(u_htmluiPostTex, u_htmluiPost_smp), uv);
}

#if defined(HAS_DROP_SHADOW)
// 1 inside the region's texels ([min, max] are their centres, extended by half a texel), 0
// outside: nothing bleeds in from neighbouring content (GLES has no CLAMP_TO_BORDER). Tested at
// the centres themselves, an integer offset read the region's first row and column exactly at
// `min` and rounding dropped them from the shadow at some pixels (it depended on the target size)
float inRegion(vec2 uv){
    vec2 halfTexel = 0.5 / vec2(textureSize(sampler2D(u_htmluiPostTex, u_htmluiPost_smp), 0));
    vec2 inside = step(htmluiPost.texCoordMinMax.xy - halfTexel, uv) * step(uv, htmluiPost.texCoordMinMax.zw + halfTexel);
    return inside.x * inside.y;
}
#endif

#ifdef HAS_BLUR
// the region's texels mirrored at its edges (Skia's mirror tile mode, which Chromium's backdrop-filter blur
// uses): a blur cut by the viewport or by the region extends the content at the edge instead of fading it out
vec2 mirrored(vec2 uv){
    vec2 halfTexel = 0.5 / vec2(textureSize(sampler2D(u_htmluiPostTex, u_htmluiPost_smp), 0));
    vec2 lo = htmluiPost.texCoordMinMax.xy - halfTexel;
    vec2 hi = htmluiPost.texCoordMinMax.zw + halfTexel;
    uv = mix(uv, 2.0 * lo - uv, step(uv, lo));
    uv = mix(uv, 2.0 * hi - uv, step(hi, uv));
    return clamp(uv, htmluiPost.texCoordMinMax.xy, htmluiPost.texCoordMinMax.zw);
}

vec4 tap(float i, float weight){
    return source(mirrored(v_uv - i * htmluiPost.params.yz)) * weight;
}
#endif

#if defined(HAS_PASSTHROUGH)
// the source, or (weights.w on) the mean of the 4 x 4 texels around v_uv
vec4 sourceBox(vec2 uv){
    if (htmluiPost.weights.w < 0.5)
        return source(uv);
    vec2 d = htmluiPost.weights.yz;
    return 0.25 * (source(uv + vec2(-d.x, -d.y)) + source(uv + vec2(d.x, -d.y)) + source(uv + vec2(-d.x, d.y)) + source(uv + vec2(d.x, d.y)));
}
#endif

#if defined(HAS_DROP_SHADOW)
float shadowAlpha(vec2 uv){
    return source(uv).a * inRegion(uv);
}

// the shadow's coverage, or (weights.w on) its mean over the 4 x 4 texels around v_uv
float shadowBox(vec2 uv){
    if (htmluiPost.weights.w < 0.5)
        return shadowAlpha(uv);
    vec2 d = htmluiPost.weights.yz;
    return 0.25 * (shadowAlpha(uv + vec2(-d.x, -d.y)) + shadowAlpha(uv + vec2(d.x, -d.y)) + shadowAlpha(uv + vec2(-d.x, d.y)) +
                   shadowAlpha(uv + vec2(d.x, d.y)));
}
#endif

void main() {
#if defined(HAS_PASSTHROUGH)
    g_finalColor = sourceBox(v_uv) * htmluiPost.params.x;
    if (htmluiPost.params.w > 0.5){
        vec2 p = v_dst * htmluiPost.params.yz;
        vec2 inside = clamp(min(p - htmluiPost.texCoordMinMax.xy, htmluiPost.texCoordMinMax.zw - p) + 0.5, 0.0, 1.0);
        g_finalColor *= inside.x * inside.y;
    }
#elif defined(HAS_COLOR_MATRIX)
    // without alpha transforms the matrix applies to premultiplied colour directly; the
    // constant column is then scaled by alpha
    vec4 color = source(v_uv);
    g_finalColor = vec4((htmluiPost.colorMatrix * color).rgb, color.a);
#elif defined(HAS_BLEND_MASK)
    float mask = texture(sampler2D(u_htmluiMaskTex, u_htmluiMask_smp), v_uv).a;
    g_finalColor = source(v_uv) * mask;
#elif defined(HAS_DROP_SHADOW)
    g_finalColor = htmluiPost.color * shadowBox(v_uv);
#elif defined(HAS_BLUR)
    vec4 w = htmluiPost.weights;
    g_finalColor = tap(-3.0, w.w) + tap(-2.0, w.z) + tap(-1.0, w.y) + tap(0.0, w.x) +
                   tap(1.0, w.y) + tap(2.0, w.z) + tap(3.0, w.w);
#elif defined(HAS_UNPREMULTIPLY)
    vec4 color = source(v_uv);
    g_finalColor = vec4(color.rgb / max(color.a, 1e-5), color.a);
#elif defined(HAS_YUV)
    vec2 chromaUv = v_uv * htmluiPost.params.yz;
    vec4 yuv = vec4(source(v_uv).r,
                    texture(sampler2D(u_htmluiYuvU, u_htmluiYuvU_smp), chromaUv).r,
                    texture(sampler2D(u_htmluiYuvV, u_htmluiYuvV_smp), chromaUv).r, 1.0);
    g_finalColor = vec4(clamp((htmluiPost.colorMatrix * yuv).rgb, 0.0, 1.0), 1.0);
#else
    g_finalColor = source(v_uv);
#endif
}
