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
//   HAS_BLEND_MODE     mix-blend-mode params.x (1 multiply .. 15 luminosity): the source's part of source-over the
//                      backdrop copy (u_htmluiMaskTex, in the source's pixels) with Compositing and Blending 1's
//                      blend function, drawn with premultiplied blending (which adds the backdrop's part); params.w
//                      on, clipped as the passthrough composite
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
#if defined(HAS_BLEND_MASK) || defined(HAS_BLEND_MODE)
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

#ifdef HAS_BLEND_MODE
// Compositing and Blending 1, §10 (the blend functions on non-premultiplied colour) and §5 (source-over)
float blendLum(vec3 c){
    return dot(c, vec3(0.3, 0.59, 0.11));
}

vec3 blendClipColor(vec3 c){
    float l = blendLum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0)
        c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0)
        c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}

vec3 blendSetLum(vec3 c, float l){
    return blendClipColor(c + (l - blendLum(c)));
}

float blendSat(vec3 c){
    return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
}

vec3 blendSetSat(vec3 c, float s){
    float mx = max(max(c.r, c.g), c.b);
    float mn = min(min(c.r, c.g), c.b);
    return mx > mn ? (c - mn) * s / (mx - mn) : vec3(0.0);
}

vec3 blendHardLight(vec3 cb, vec3 cs){
    vec3 multiply = cb * (2.0 * cs);
    vec3 s = 2.0 * cs - 1.0;
    vec3 screen = cb + s - cb * s;
    return mix(screen, multiply, step(cs, vec3(0.5)));
}

vec3 blendColorDodge(vec3 cb, vec3 cs){
    vec3 r = min(vec3(1.0), cb / max(1.0 - cs, 1e-6));
    r = mix(r, vec3(1.0), step(1.0, cs));
    return mix(r, vec3(0.0), step(cb, vec3(0.0)));
}

vec3 blendColorBurn(vec3 cb, vec3 cs){
    vec3 r = 1.0 - min(vec3(1.0), (1.0 - cb) / max(cs, 1e-6));
    r = mix(r, vec3(0.0), step(cs, vec3(0.0)));
    return mix(r, vec3(1.0), step(1.0, cb));
}

vec3 blendSoftLight(vec3 cb, vec3 cs){
    vec3 d = mix(sqrt(cb), ((16.0 * cb - 12.0) * cb + 4.0) * cb, step(cb, vec3(0.25)));
    vec3 low = cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
    vec3 high = cb + (2.0 * cs - 1.0) * (d - cb);
    return mix(high, low, step(cs, vec3(0.5)));
}

vec3 blendFunction(int mode, vec3 cb, vec3 cs){
    if (mode == 1) return cb * cs;
    if (mode == 2) return cb + cs - cb * cs;
    if (mode == 3) return blendHardLight(cs, cb);
    if (mode == 4) return min(cb, cs);
    if (mode == 5) return max(cb, cs);
    if (mode == 6) return blendColorDodge(cb, cs);
    if (mode == 7) return blendColorBurn(cb, cs);
    if (mode == 8) return blendHardLight(cb, cs);
    if (mode == 9) return blendSoftLight(cb, cs);
    if (mode == 10) return abs(cb - cs);
    if (mode == 11) return cb + cs - 2.0 * cb * cs;
    if (mode == 12) return blendSetLum(blendSetSat(cs, blendSat(cb)), blendLum(cb));
    if (mode == 13) return blendSetLum(blendSetSat(cb, blendSat(cs)), blendLum(cb));
    if (mode == 14) return blendSetLum(cs, blendLum(cb));
    if (mode == 15) return blendSetLum(cb, blendLum(cs));
    return cs;
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
#elif defined(HAS_BLEND_MODE)
    vec4 s = source(v_uv);
    vec4 b = texture(sampler2D(u_htmluiMaskTex, u_htmluiMask_smp), v_uv);
    vec3 cs = s.rgb / max(s.a, 1e-5);
    vec3 cb = b.rgb / max(b.a, 1e-5);
    vec3 blended = clamp(blendFunction(int(htmluiPost.params.x + 0.5), cb, cs), 0.0, 1.0);
    // source-over's backdrop term, b * (1 - s.a), is the premultiplied blend state's: where the source has no coverage
    // the destination is left as it is
    g_finalColor = vec4(s.rgb * (1.0 - b.a) + s.a * b.a * blended, s.a);
    // the composite's clip, as the passthrough composite has it (S92)
    if (htmluiPost.params.w > 0.5){
        vec2 p = v_dst * htmluiPost.params.yz;
        vec2 inside = clamp(min(p - htmluiPost.texCoordMinMax.xy, htmluiPost.texCoordMinMax.zw - p) + 0.5, 0.0, 1.0);
        g_finalColor *= inside.x * inside.y;
    }
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
