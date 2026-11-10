// S83's light effects (§6.6.1 Light Glow, §6.6.2 Contour Lines, §6.6.24 Neon Tube). Light Glow
// and Contour Lines are ported from Pachislop src/effects/a/lightGlow.ts and glow.ts (deepGlow,
// contourLines) by SafEight, used with the author's permission; Neon Tube is an original design
// on the same kernel. Every function is one TFX_PROGRAM's body; the values are the p[2..] the
// C++ side (effects/LightEffects.cpp) packs, as documented above each.

#ifndef TITLEART_FX_LIGHT_GLSL
#define TITLEART_FX_LIGHT_GLSL

// The fraction of a pixel's light above the luminance threshold, soft knee of width `knee`
// (quadratic toe, then linear): the threshold is subtracted, so only the excess emits
float tfx_emitFraction(vec4 L, float thresh, float knee) {
    if (thresh <= 0.0) return 1.0;
    float b = ta_luma(ta_unpremul(L));
    float x = b - thresh;
    float e = x;
    if (knee > 1e-4) {
        float q = clamp(x + knee, 0.0, 2.0 * knee);
        e = max(x, q * q / (4.0 * knee));
    }
    return max(e, 0.0) / max(b, 1e-4);
}

// replaces the chroma, keeps the brightest channel (a saturated colour keeps its full strength)
vec3 tfx_colorize(vec3 c, vec3 t) {
    return max(c.r, max(c.g, c.b)) * t / max(max(t.r, max(t.g, t.b)), 1e-4);
}

// The over-exposure response (lightGlow.ts lightGlowComposeFrag): exactly the identity for every
// channel <= 1 that nothing pushed up. The excess spills into its spectral neighbours (and by
// Core Whiteness into the far channel) in two steps, then a soft shoulder to 1.5; written as
// x + (y - x) k so it stays bit-exact where y == x
vec3 tfx_shoulder(vec3 x) {
    return min(x, 1.0) + 0.5 * (1.0 - exp(-2.0 * max(x - 1.0, 0.0)));
}
vec3 tfx_bleed(vec3 e, float white) {
    return 0.5 * vec3(e.g, e.r + e.b, e.g) + 0.5 * white * vec3(e.b, 0.0, e.r);
}
vec3 tfx_overexpose(vec3 x, float shift, float white) {
    vec3 x1 = x + shift * tfx_bleed(max(x - 1.0, 0.0), white);
    vec3 x2 = x + shift * tfx_bleed(max(x1 - 1.0, 0.0), white);
    return x + (tfx_shoulder(x2) - x) * smoothstep(0.0, 0.25, shift);
}

// The input's light: TA_TEX0's colour x p4.x where it holds light / p4.x (S83 D14: a group's
// light accumulator on the RGBA8 path; 0 = plain)
vec4 tfx_lightInput(vec2 uv) {
    vec4 L = texture(TA_TEX0, uv);
    if (ta.p[4].x > 0.0) L.rgb *= ta.p[4].x;
    return L;
}

// Emitter: the layer's light above the threshold x 2^exposure (premultiplied, so antialiased
// edges emit their share). TA_TEX0 L. p2 = (gain, threshold, knee, custom colour 0/1),
// p3 = (custom colour linear, 1 / headroom), p4.x the input's encoding (tfx_lightInput)
vec4 tfx_lightEmit() {
    vec4 L = tfx_lightInput(ta_uv());
    vec3 e = L.rgb * tfx_emitFraction(L, ta.p[2].y, ta.p[2].z) * ta.p[2].x;
    if (ta.p[2].w > 0.5) e = tfx_colorize(e, ta.p[3].rgb);
    return vec4(max(e, 0.0) * ta.p[3].w, 0.0);
}

// 2x reduction: four bilinear taps on the four source texels (a 2 x 2 box when the size halves
// exactly). TA_TEX0 the finer level. p2.xy = its texel size in uv
vec4 tfx_lightDown() {
    vec2 uv = ta_uv();
    vec2 h = 0.5 * ta.p[2].xy;
    return 0.25 * (texture(TA_TEX0, uv + vec2(-h.x, -h.y)) + texture(TA_TEX0, uv + vec2(h.x, -h.y)) +
                   texture(TA_TEX0, uv + vec2(-h.x, h.y)) + texture(TA_TEX0, uv + vec2(h.x, h.y)));
}

// One axis of a Gaussian with a sigma per channel (chromatic scatter); taps outside the whole
// render read zero, so light leaves the picture instead of piling up at its edges.
// TA_TEX0 the level. p2 = (uv step per tap, taps <= 48), p3.rgb = sigma per channel, level texels
vec4 tfx_lightBlur() {
    vec2 uv = ta_uv();
    vec2 dir = ta.p[2].xy;
    int taps = int(ta.p[2].z + 0.5);
    vec3 k = 0.5 / max(ta.p[3].rgb * ta.p[3].rgb, vec3(1e-4));
    vec4 frame = ta_frameUv();
    vec3 acc = vec3(0.0), ws = vec3(0.0);
    for (int i = 0; i <= 96; i++) {
        int j = i - taps;
        if (j > taps) break;
        float f = float(j);
        vec3 w = exp(-f * f * k);
        ws += w;
        vec2 q = uv + dir * f;
        if (q.x >= frame.x && q.x <= frame.z && q.y >= frame.y && q.y <= frame.w) acc += textureLod(TA_TEX0, q, 0.0).rgb * w;
    }
    return vec4(acc / ws, 0.0);
}

// acc + weight x the octave (tinted toward the inner -> outer tint at its radius). TA_TEX0 the
// octave (any level, sampled by uv), TA_TEX1 the accumulation so far. p2 = (tint linear, weight),
// p3 = (tint amount, 1 = the first octave (no accumulation yet), 1 = the accumulation so far is
// a coarser level, sampled bilinearly: the mobile cascade of D11)
vec4 tfx_lightAccum() {
    vec2 uv = ta_uv();
    vec3 c = texture(TA_TEX0, uv).rgb;
    if (ta.p[3].x > 0.0) c = mix(c, tfx_colorize(c, ta.p[2].rgb), ta.p[3].x);
    vec3 prev = vec3(0.0);
    if (ta.p[3].y < 0.5) prev = ta.p[3].z > 0.5 ? texture(TA_TEX1, uv).rgb : texelFetch(TA_TEX1, ivec2(gl_FragCoord.xy), 0).rgb;
    return vec4(prev + c * ta.p[2].w, 0.0);
}

// Direct light of the layer (its emitting part x 2^exposure) + the scattered glow (kept off the
// face by On Face) through the over-exposure response, alpha encoding the added light.
// TA_TEX0 L, TA_TEX1 the glow. p2 = (gain, threshold, knee, on face), p3 = (highlight shift,
// core whiteness, headroom x glow scale (S81's scale_split multiplies it per frame)), p4.x the
// input's encoding (tfx_lightInput)
vec4 tfx_lightCompose() {
    vec2 uv = ta_uv();
    vec4 L = tfx_lightInput(uv);
    vec3 g = texture(TA_TEX1, uv).rgb * ta.p[3].z * (1.0 - (1.0 - ta.p[2].w) * L.a);
    vec3 direct = L.rgb * (1.0 + tfx_emitFraction(L, ta.p[2].y, ta.p[2].z) * (ta.p[2].x - 1.0));
    vec3 c = tfx_overexpose(max(direct + g, 0.0), ta.p[3].x, ta.p[3].y);
    float la = clamp(max(max(c.r, c.g), c.b), 0.0, 1.0);
    return vec4(c, max(L.a, la));
}

// Contour lines (疑似導光板): iso-lines of the traced ink's distance at offset + k x spacing,
// antialiased by width, weighted by falloff over the line index, coloured across the layer's box
// or by line number. TA_RAW0 the distance, TA_RAW1 the bounds, TA_TEX3 the ramp.
// p2 = (spacing px, width px, offset px, falloff), p3 = (count, lines 0 inside 1 outside 2 both,
// colour by 0 across 1 line number), p4.xy = the gradient direction (frame px, y down)
vec4 tfx_contourLines() {
    float d = fx_readDistance(ivec2(gl_FragCoord.xy));
    float spacing = ta.p[2].x, width = ta.p[2].y, offset = ta.p[2].z, falloff = ta.p[2].w;
    float count = ta.p[3].x;
    int mode = int(ta.p[3].y + 0.5), colorBy = int(ta.p[3].z + 0.5);
    float depth = mode == 0 ? -d : mode == 1 ? d : abs(d);
    float k = clamp(floor((depth - offset) / spacing + 0.5), 0.0, count - 1.0);
    float dist = abs(depth - offset - k * spacing);
    float cov = clamp(0.5 * width - dist + 0.5, 0.0, 1.0);
    float fade = count > 1.0 ? 1.0 - falloff * k / (count - 1.0) : 1.0;
    float t;
    if (colorBy == 0) {
        vec2 dir = ta.p[4].xy;
        vec4 b = fx_layerBox();
        vec2 c = 0.5 * (b.xy + b.zw), hs = 0.5 * (b.zw - b.xy);
        float ext = max(abs(dir.x) * hs.x + abs(dir.y) * hs.y, 1.0);
        t = 0.5 + dot(gl_FragCoord.xy - c, dir) / (2.0 * ext);
    } else {
        t = count > 1.0 ? k / (count - 1.0) : 0.0;
    }
    vec4 g = texture(TA_TEX3, vec2(clamp(t, 0.0, 1.0), 0.5));
    float a = cov * fade * g.a;
    return vec4(ta_toLinear(g.rgb) * a, a);
}

// The lines and their glow added to the (optionally dimmed) layer as light.
// TA_TEX0 L, TA_TEX1 the lines, TA_TEX2 their glow. p2 = (intensity, glow amount, dim face)
vec4 tfx_contourCompose() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec4 base = L * vec4(vec3(1.0 - ta.p[2].z), 1.0);
    vec3 light = max((texture(TA_TEX1, uv).rgb + texture(TA_TEX2, uv).rgb * ta.p[2].y) * ta.p[2].x, 0.0);
    return tfx_addLight(base, light);
}

// Neon Tube: a glass tube of `width` along the silhouette (centred `offset` px from the edge, +
// outside) or filling the layer's own thin shape, shaded as a lit cylinder with a white-hot core;
// broken by gaps and dimmed by flicker along the contour (noise at the nearest edge point, so the
// pattern runs along the tube, not across it). TA_RAW0 the content's distance.
// p2 = (width px, offset px, source 0 outline 1 fill, core), p3 = (colour linear, flicker),
// p4 = (core colour linear, gaps), p5 = (light direction (frame px, y down), seed, 1 / headroom),
// p6.xy = the canvas centre (target px)
vec4 tfx_neonTube() {
    vec2 p = gl_FragCoord.xy;
    float d = tfx_distanceAt(ivec2(p));
    float hw = max(0.5 * ta.p[2].x, 0.5);
    bool fill = ta.p[2].z > 0.5;
    float centre = fill ? -hw : ta.p[2].y;
    float x = (d - centre) / hw;
    float cover = clamp((1.0 - abs(x)) * hw + 0.5, 0.0, 1.0);
    if (cover <= 0.0) return vec4(0.0);
    x = clamp(x, -1.0, 1.0);
    vec2 n = tfx_distanceNormal(p);
    // the nearest edge point, in canvas px: noise there runs along the contour
    vec2 q = tfx_canvasPx(p - n * d, ta.p[6].xy);
    float seed = ta.p[5].z;
    float widthCanvas = 2.0 * hw / TA_PX;
    float gaps = ta.p[4].w;
    float keep = 1.0;
    if (gaps > 0.0) {
        float g = vnoise(q / (6.0 * widthCanvas) + vec2(seed * 7.31, seed * 3.17));
        keep = smoothstep(gaps - 0.04, gaps + 0.04, g);
    }
    float flicker = ta.p[3].w;
    float bright = 1.0;
    if (flicker > 0.0) {
        float seg = hash12(floor(q / (12.0 * widthCanvas)) + vec2(seed * 1.37, seed * 9.11));
        bright = 1.0 - flicker * smoothstep(0.35, 0.65, seg) * (0.55 + 0.45 * hash12(vec2(seed, seg * 91.7)));
    }
    // a cylinder across the band: its normal turns from the edge normal at x = +-1 to the viewer
    float nz = sqrt(max(1.0 - x * x, 0.0));
    vec3 N = normalize(vec3(n * x, nz + 1e-3));
    vec3 Ld = normalize(vec3(ta.p[5].xy, 0.9));
    float diffuse = 0.55 + 0.45 * max(dot(N, Ld), 0.0);
    float spec = pow(max(dot(reflect(-Ld, N), vec3(0.0, 0.0, 1.0)), 0.0), 24.0);
    vec3 color = ta.p[3].rgb, core = ta.p[4].rgb;
    vec3 tube = color * diffuse + core * ta.p[2].w * 2.5 * pow(nz, 6.0) + vec3(spec * 0.6);
    float a = cover * keep;
    return vec4(tube * a * bright * ta.p[5].w, a);
}

// The tube over the layer, its glow added as light, through the over-exposure response so the
// core whitens. TA_TEX0 L, TA_TEX1 the tube, TA_TEX2 its glow. p2 = (glow amount, headroom,
// highlight shift, core whiteness)
vec4 tfx_neonCompose() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    float H = ta.p[2].y;
    vec4 tube = texture(TA_TEX1, uv);
    tube.rgb *= H;
    vec3 g = texture(TA_TEX2, uv).rgb * H * ta.p[2].x;
    vec3 base = L.rgb * (1.0 - tube.a) + tube.rgb;
    float a = L.a + tube.a * (1.0 - L.a);
    vec3 c = tfx_overexpose(max(base + g, 0.0), ta.p[2].z, ta.p[2].w);
    float la = clamp(max(max(c.r, c.g), c.b), 0.0, 1.0);
    return vec4(c, max(a, la));
}

#endif // TITLEART_FX_LIGHT_GLSL
