// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/shaders.ts (blendMaterial, adjustMaterial, outputMaterial),
// src/engine/masks.ts (the gradient and combine materials) and src/effects/lib.ts (blur) by
// SafEight, used with the author's permission.

// The compositor's passes: copies and fixed-function composes, the shader blend modes, the
// adjustment layers, masks, the RGBA blur and the output. A texture is read through a uv map
// vec4(scale.xy, offset.xy): uv = gl_FragCoord.xy * scale + offset, so a pass reads a target
// of another size or position (a node target drawn into its parent at its offset, a level of a
// blur pyramid) without vertex maths.

vec2 taMapUv(vec4 map) { return gl_FragCoord.xy * map.xy + map.zw; }
bool taInsideUv(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }

#if TA_PROGRAM == TA_PROGRAM_COPY
// TA_TEX0 through p[2] x opacity p[3].x; p[3].y 0 rgba, 1 the red channel as a value (r r r r);
// p[3].z 1: transparent outside the source; p[3].w mask 0 none, 1 TA_TEX1's alpha, 2 its red,
// through p[4]; p[5].w 1: S85's layer tint p[5].rgb (multiply) and flash p[6].rgb (added under
// the alpha), linear, on the premultiplied source
void main() {
    vec2 uv = taMapUv(ta.p[2]);
    vec4 s = texture(TA_TEX0, uv);
    if (ta.p[3].y > 0.5) s = vec4(s.r);
    if (ta.p[3].z > 0.5 && !taInsideUv(uv)) s = vec4(0.0);
    if (ta.p[5].w > 0.5) s.rgb = max(s.rgb * ta.p[5].rgb + ta.p[6].rgb * s.a, vec3(0.0));
    s *= ta.p[3].x;
    if (ta.p[3].w > 0.5) {
        vec4 m = texture(TA_TEX1, taMapUv(ta.p[4]));
        s *= ta.p[3].w > 1.5 ? m.r : m.a;
    }
    frag_color = s;
}
#endif

#if TA_PROGRAM == TA_PROGRAM_BLEND
// mode p[5].x of TA_TEX1 (through p[2], transparent outside it) x opacity p[5].y over the
// backdrop copy TA_TEX0 (through p[3]); p[5].z 1: inside TA_TEX2's alpha (the clip group's base
// content), through p[4]; p[6].w 1: S85's layer tint p[6].rgb and flash p[7].rgb (as COPY's)
void main() {
    vec2 su = taMapUv(ta.p[2]);
    vec4 s = taInsideUv(su) ? texture(TA_TEX1, su) : vec4(0.0);
    if (ta.p[6].w > 0.5) s.rgb = max(s.rgb * ta.p[6].rgb + ta.p[7].rgb * s.a, vec3(0.0));
    s *= ta.p[5].y;
    if (ta.p[5].z > 0.5) s *= texture(TA_TEX2, taMapUv(ta.p[4])).a;
    vec4 d = texture(TA_TEX0, taMapUv(ta.p[3]));
    frag_color = taBlend(d, s, int(ta.p[5].x + 0.5));
}
#endif

#if TA_PROGRAM == TA_PROGRAM_ADJUST_MIX
// an adjustment layer: mix(below TA_TEX0, blend(below, adjusted TA_TEX1), k) on sRGB-encoded
// straight colour, k = opacity p[2].x x the clip group's content alpha (TA_TEX2 through p[4],
// when p[2].z) x the layer masks (TA_TEX3's red through p[5], when p[2].w); p[2].y the blend
// mode; both colour targets through p[3]. Where k is 0 the backdrop passes untouched; the
// alpha follows the adjusted target's (its effects may change the coverage), Pachislop's
// adjustMaterial.
void main() {
    vec2 uv = taMapUv(ta.p[3]);
    vec4 d = texture(TA_TEX0, uv);
    float k = ta.p[2].x;
    if (ta.p[2].z > 0.5) k *= texture(TA_TEX2, taMapUv(ta.p[4])).a;
    if (ta.p[2].w > 0.5) k *= texture(TA_TEX3, taMapUv(ta.p[5])).r;
    vec4 a = texture(TA_TEX1, uv);
    if (k <= 0.0 || (d.a <= 0.0 && a.a <= 0.0)) { frag_color = d; return; }
    vec3 cb = taToSrgb(d.a > 0.0 ? d.rgb / d.a : vec3(0.0));
    vec3 ca = taToSrgb(a.a > 0.0 ? a.rgb / a.a : vec3(0.0));
    float w = a.a > 0.0 ? min(1.0, d.a / a.a) : 1.0;
    vec3 cr = mix(ca, taBlendFn(cb, ca, int(ta.p[2].y + 0.5)), w);
    float oa = mix(d.a, a.a, k);
    frag_color = oa > 0.0 ? vec4(taToLinear(mix(cb * d.a, cr * a.a, k) / oa) * oa, oa) : vec4(0.0);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_MASK_VALUE
// one mask's value from TA_TEX0 (through p[3]): p[2].x 0 its red, 1 its alpha, 2 the luminance
// of its sRGB-encoded straight colour x alpha (a designer's matte); p[2].y invert; p[2].z
// density; p[2].w 1: the first mask, else times the masks so far (TA_TEX1 through p[4])
void main() {
    vec4 c = texture(TA_TEX0, taMapUv(ta.p[3]));
    float a = clamp(c.a, 0.0, 1.0);
    int channel = int(ta.p[2].x + 0.5);
    float v = channel == 0 ? clamp(c.r, 0.0, 1.0)
            : channel == 1 ? a
            : a > 0.0 ? taLuma709(taToSrgb(c.rgb / a)) * a : 0.0;
    if (ta.p[2].y > 0.5) v = 1.0 - v;
    v = 1.0 - ta.p[2].z * (1.0 - v);
    frag_color = vec4(ta.p[2].w > 0.5 ? v : texture(TA_TEX1, taMapUv(ta.p[4])).r * v);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_MASK_GRADIENT
// an analytic gradient mask: opaque at the start (or inside length x inner), transparent at
// the length, half at `mid` (t^gamma = 1/2). p[2], p[3]: the target pixel -> mask space affine
// (x = dot(p[2].xy, f) + p[2].z, y = dot(p[3].xy, f) + p[3].z); p[4] start, direction; p[5]
// length, inner, gamma, radial
void main() {
    vec2 f = gl_FragCoord.xy;
    vec2 q = vec2(dot(ta.p[2].xy, f) + ta.p[2].z, dot(ta.p[3].xy, f) + ta.p[3].z) - ta.p[4].xy;
    float len = ta.p[5].x, inner = ta.p[5].y;
    float t = ta.p[5].w > 0.5 ? (length(q) - inner * len) / max(len * (1.0 - inner), 1e-4) : dot(q, ta.p[4].zw) / max(len, 1e-4);
    frag_color = vec4(1.0 - pow(clamp(t, 0.0, 1.0), ta.p[5].z));
}
#endif

#if TA_PROGRAM == TA_PROGRAM_BLUR
// one separable Gaussian pass of TA_TEX0 (through p[3]): p[2].xy the uv step of one tap, p[2].z
// sigma in taps (the radius sets the loop, at most 24 taps a side: larger blurs run on a
// pyramid level, Compositor's blur())
void main() {
    vec2 uv = taMapUv(ta.p[3]);
    float sigma = ta.p[2].z;
    if (sigma < 0.01) { frag_color = texture(TA_TEX0, uv); return; }
    int r = min(int(ceil(sigma * 3.0)), 24);
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int i = -r; i <= r; i++) {
        float w = exp(-0.5 * float(i * i) / (sigma * sigma));
        acc += texture(TA_TEX0, uv + ta.p[2].xy * float(i)) * w;
        wsum += w;
    }
    frag_color = acc / wsum;
}
#endif

#if TA_PROGRAM == TA_PROGRAM_OUTPUT
// linear premultiplied TA_TEX0 (through p[3]) -> the output: p[2].x 0 straight sRGB with an
// interleaved-gradient-noise dither under half a code (8-bit outputs; exact codes never move),
// 1 straight sRGB (16-bit), 2 premultiplied sRGB (displays), 3 the alpha as opaque grey (mask
// outputs), 4 light (glow outputs: the colour's largest channel as alpha, so the image drawn
// additively gives the light back), dithered like 0; p[2].yz the output pixel of this target's
// (0, 0), so the noise is the whole render's whatever the region; p[2].w scales mode 4's light
// (RenderRequest::lightScale: check_title reads light above 1 / scale as full codes)
float taIgn(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
void main() {
    vec4 c = texture(TA_TEX0, taMapUv(ta.p[3]));
    int mode = int(ta.p[2].x + 0.5);
    if (mode == 4) {
        c.rgb = max(c.rgb, vec3(0.0)) * (ta.p[2].w > 0.0 ? ta.p[2].w : 1.0);
        c.a = max(c.r, max(c.g, c.b));
        mode = 0;
    }
    float a = clamp(c.a, 0.0, 1.0);
    if (mode == 3) { frag_color = vec4(vec3(a), 1.0); return; }
    vec3 s = taToSrgb(a > 0.0 ? c.rgb / a : vec3(0.0));
    if (mode == 2) { frag_color = vec4(s * a, a); return; }
    if (mode == 0 && a > 0.0) {
        vec2 px = gl_FragCoord.xy + ta.p[2].yz;
        s += (taIgn(px) - 0.5) * (0.98 / 255.0);
        if (a < 1.0) a = clamp(a + (taIgn(px + vec2(47.0, 17.0)) - 0.5) * (0.98 / 255.0), 0.0, 1.0);
    }
    frag_color = vec4(s, a);
}
#endif

#if TA_PROGRAM == TA_PROGRAM_GLYPH_COMPOSE
// S81's Glyph Atlas compose (docs/specs/S81-title-art-runtime.md R28): up to 20 cell quads of
// the atlas page TA_TEX0, each over the ones before it (premultiplied), the whole drawn over the
// target with premultiplied blending. p[2].x the count; quad i: p[3 + 2i] its rectangle in this
// target's pixels (x0, y0, x1, y1 in gl_FragCoord space), p[4 + 2i] its cell in the page (u0, v0,
// u1, v1). The cells hold linear premultiplied colour (the accumulators' encoding).
void main() {
    vec2 f = gl_FragCoord.xy;
    int count = int(ta.p[2].x + 0.5);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 20; i++) {
        if (i >= count) break;
        vec4 r = ta.p[3 + 2 * i];
        if (f.x < r.x || f.y < r.y || f.x >= r.z || f.y >= r.w) continue;
        vec4 u = ta.p[4 + 2 * i];
        vec2 t = (f - r.xy) / max(r.zw - r.xy, vec2(1e-3));
        vec4 s = texture(TA_TEX0, mix(u.xy, u.zw, t));
        acc = s + acc * (1.0 - s.a);
    }
    frag_color = acc;
}
#endif

#if TA_PROGRAM == TA_PROGRAM_GLYPH_COMPOSE_ANIM
// S85 R15: up to 7 cell quads of the atlas page TA_TEX0 with a per-glyph pose, each over the ones
// before it (premultiplied). p[2].x the count; quad i: p[3 + 6i] the moved quad's bounds in this
// target's pixels; p[4 + 6i] its cell in the page (u0, v0, u1, v1); p[5 + 6i].xyz and p[6 + 6i].xyz
// the affine from those pixels to the cell's unit square, p[5 + 6i].w the opacity; p[7 + 6i].rgb
// the tint and p[8 + 6i].rgb the flash (colour x amount), linear. The tint multiplies and the
// flash adds, so a glyph at (1, 1, 1) tint and 0 flash is the compose's own colour.
void main() {
    vec2 f = gl_FragCoord.xy;
    int count = int(ta.p[2].x + 0.5);
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 7; i++) {
        if (i >= count) break;
        vec4 r = ta.p[3 + 6 * i];
        if (f.x < r.x || f.y < r.y || f.x >= r.z || f.y >= r.w) continue;
        vec4 row0 = ta.p[5 + 6 * i];
        vec4 row1 = ta.p[6 + 6 * i];
        vec2 t = vec2(dot(row0.xyz, vec3(f, 1.0)), dot(row1.xyz, vec3(f, 1.0)));
        if (t.x < 0.0 || t.y < 0.0 || t.x >= 1.0 || t.y >= 1.0) continue;
        vec4 u = ta.p[4 + 6 * i];
        vec4 s = texture(TA_TEX0, mix(u.xy, u.zw, t));
        s.rgb = s.rgb * ta.p[7 + 6 * i].rgb + ta.p[8 + 6 * i].rgb * s.a;
        s *= row0.w;
        acc = s + acc * (1.0 - s.a);
    }
    frag_color = acc;
}
#endif
