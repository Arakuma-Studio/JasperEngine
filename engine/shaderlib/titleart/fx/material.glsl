// S83's Material effect (§6.6.16): a 2D title Material shading the input's shape, after Pachislop
// src/effects/b/material.ts by SafEight, used with the author's permission. The element program
// of the renderer (titleart/render/element.glsl, ported from Pachislop src/engine/layer2d/
// shaders.ts) needs more parameters and images than an effect pass binds, so the effect shades in
// two passes with that program's formulas: the surface (fill, bevel light through the gloss
// contour, a surface texture, finish) and the details over it (pattern, satin, colour texture,
// sheen, inner glow, gloss cap). The colour maths runs on sRGB-encoded values, like Photoshop.
// Frame px, y down: normals and the light are in the same frame.

#ifndef TITLEART_FX_MATERIAL_GLSL
#define TITLEART_FX_MATERIAL_GLSL

vec3 tfx_screen(vec3 b, vec3 s) {
    return 1.0 - (1.0 - b) * (1.0 - s);
}

// the bevel profiles of S80 (fields.glsl taProfile): 0 smooth 1 button 2 mesa 3 chamfer 4 ridge
// 5 cove 6 stepped (7 custom shades as smooth)
float tfx_profile(float t, int profile) {
    if (profile == 1) { float u = 1.0 - t; return 1.0 - u * u * u; }
    if (profile == 2) { float u = 1.0 - min(t / 0.45, 1.0); return 1.0 - u * u; }
    if (profile == 3) return t;
    if (profile == 4) return 1.0 - abs(2.0 * t - 1.0);
    if (profile == 5) return t * t;
    if (profile == 6) { float f = min(t, 0.9999) * 3.0; return (floor(f) + smoothstep(0.55, 1.0, fract(f))) / 3.0; }
    return sin(t * 1.5707963);
}

// the gloss contours of S80 (element.glsl contour): 0 linear 1 ring 2 double ring 3 cove
// 4 rounded steps 5 sawtooth (6 custom reads the curve's lookup in TA_TEX1)
float tfx_contour(float x, int gloss) {
    x = clamp(x, 0.0, 1.0);
    if (gloss == 6) return texture(TA_TEX1, ta_lutUv(x)).r;
    if (gloss == 1) { float s = sin(x * 2.6); return s * s; }
    if (gloss == 2) { float s = sin(x * 5.5); return s * s; }
    if (gloss == 3) return 1.0 - sqrt(max(1.0 - x * x, 0.0));
    if (gloss == 4) { float f = min(x, 0.9999) * 4.0; return (floor(f) + smoothstep(0.3, 0.7, fract(f))) / 4.0; }
    if (gloss == 5) return fract(min(x, 0.9999) * 3.0) * (0.6 + 0.4 * x);
    return x;
}

// smooth 1D value noise (the metal grains)
float tfx_metalN1(float x, float seed) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash12(vec2(i, seed)), hash12(vec2(i + 1.0, seed)), f);
}

// the texture kinds' height-like value, 0.5 neutral (element.glsl textureHeight, the procedural
// kinds): 1 noise 2 sparkle 3 brushed 4 ripple 5 grunge 6 clouds 7 hammered 8 crumple 9 liquid
// 10 hairline 11 spun 12 flakes; the relief kinds (7-9) as noise of their scale. q in feature
// units (the texture's own axes), c the box centre in them
float tfx_textureHeight(int kind, vec2 q, vec2 c) {
    if (kind == 1) return clamp(0.5 + (vnoise(q * 2.0) - 0.5) * 1.3, 0.0, 1.0);
    if (kind == 2) {
        vec2 cell = floor(q * 0.5);
        float best = 0.0;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            vec2 cc = cell + vec2(x, y);
            if (hash12(cc + 7.3) > 0.45) continue;
            vec2 d = q * 0.5 - (cc + hash22(cc));
            float size = 0.25 + 0.75 * hash12(cc + 3.1);
            best = max(best, exp(-dot(d, d) / (0.012 * size)) * size);
        }
        return 0.5 + 0.5 * min(best, 1.0);
    }
    if (kind == 3) return clamp(0.5 + (fbm(vec2(q.x * 0.06, q.y * 2.5)) - 0.5) * 1.6, 0.0, 1.0);
    if (kind == 4) return 0.5 + 0.45 * sin(6.2831853 * 2.5 * fbm(q * 0.35));
    if (kind == 5) {
        float blot = smoothstep(0.5, 0.72, fbm(q * 0.45));
        float speck = step(0.93, hash12(floor(q * 3.0)));
        return clamp(0.5 - blot * 0.45 - speck * 0.25, 0.0, 1.0);
    }
    if (kind == 10) {
        float lines = 0.55 * tfx_metalN1(q.y, 1.0) + 0.3 * tfx_metalN1(q.y * 2.7, 2.0) + 0.15 * hash12(vec2(floor(q.y * 4.0), 3.0));
        return mix(lines, lines * vnoise(vec2(q.x / 90.0, q.y * 0.21)) * 1.6, 0.35);
    }
    if (kind == 11) {
        vec2 dv = q - c;
        float ring = length(dv);
        float lines = 0.6 * tfx_metalN1(ring, 4.0) + 0.4 * tfx_metalN1(ring * 3.1, 5.0);
        return lines * (0.75 + 0.5 * vnoise(dv / max(ring, 1e-3) * 3.0 + floor(ring) * 0.13));
    }
    if (kind == 12) {
        vec3 w = tfx_worley(q, 0.0);
        return 0.3 + 0.7 * w.z;
    }
    return clamp(0.5 + (fbm(q * 0.25) - 0.5) * 1.6, 0.0, 1.0);
}

// Material surface: the fill (a solid, a gradient across the layer box or the shape burst, or
// the layer's own colours) lit by the bevel (the height from the distance through the profile,
// Direction Down pressed in, a surface texture bending it) through the gloss contour, then the
// finish (a gradient map of the lit luma). TA_TEX0 the input, TA_TEX1 the gloss curve's lookup,
// TA_TEX2 the fill ramp, TA_TEX3 the finish ramp, TA_RAW0 the distance, TA_RAW1 the bounds.
// p2 = (fill kind 0 solid 1 linear 2 radial 3 angle 4 reflected 5 diamond 6 shape burst, fill
// scale, fill offset, layer colours 0/1), p3 = (fill colour sRGB, opacity), p4 = (fill direction
// (y down), bevel on, bevel size px), p5 = (depth, profile, direction 1 up -1 down, soften px),
// p6 = (light (frame x, y down, z toward the viewer), gloss contour), p7 = (highlight colour
// sRGB, opacity), p8 = (shadow colour sRGB, opacity), p9 = (finish on, finish amount, surface
// texture kind (0 none), its amount), p10 = (texture feature px, texture direction (y down),
// shape-burst depth px), p11.xy = the texture seed offset
float tfx_heightAt(vec2 p, float size, int profile, float dirSign) {
    float d = tfx_distanceSmooth(p);
    float h = tfx_profile(clamp(-d / max(size, 0.5), 0.0, 1.0), profile);
    return dirSign < 0.0 ? 1.0 - h : h;
}
vec4 tfx_materialSurface() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    if (L.a <= 0.0) return vec4(0.0);
    int fillKind = int(ta.p[2].x + 0.5);
    bool layerColours = ta.p[2].w > 0.5;
    bool bevel = ta.p[4].z > 0.5;
    float size = ta.p[4].w;
    float depth = ta.p[5].x;
    int profile = int(ta.p[5].y + 0.5);
    float dirSign = ta.p[5].z;
    float soften = max(ta.p[5].w, 1.0);
    vec3 Ld = ta.p[6].xyz;
    int gloss = int(ta.p[6].w + 0.5);
    int texKind = int(ta.p[9].z + 0.5);
    float texAmt = ta.p[9].w;
    float texPx = max(ta.p[10].x, 0.5);
    vec2 texDir = ta.p[10].yz;
    vec2 tq = vec2(dot(p, texDir), dot(p, vec2(-texDir.y, texDir.x))) / texPx + ta.p[11].xy;
    vec4 b = fx_layerBox();
    vec2 bc = 0.5 * (b.xy + b.zw), hs = max(0.5 * (b.zw - b.xy), vec2(0.5));
    vec2 tc = vec2(dot(bc, texDir), dot(bc, vec2(-texDir.y, texDir.x))) / texPx + ta.p[11].xy;
    // the fill
    vec3 c;
    float fillA = 1.0;
    if (layerColours) {
        c = ta_toSrgb(ta_unpremul(L));
    } else if (fillKind == 0) {
        c = ta.p[3].rgb;
    } else {
        vec2 v = p - bc;
        vec2 dir = ta.p[4].xy;
        float s = max(ta.p[2].y, 1e-3);
        float ext = max(abs(dir.x) * hs.x + abs(dir.y) * hs.y, 1e-3);
        float t;
        if (fillKind == 1) t = dot(v, dir) / (2.0 * ext * s) + 0.5;
        else if (fillKind == 2) t = length(v) / (max(length(hs), 1e-3) * s);
        else if (fillKind == 3) {
            float a = atan(dir.x * v.y - dir.y * v.x, dot(dir, v));
            t = (a < 0.0 ? a + 6.2831853 : a) / (6.2831853 * s);
        } else if (fillKind == 4) t = abs(dot(v, dir)) / (ext * s);
        else if (fillKind == 5) t = (abs(v.x) + abs(v.y)) / (max(hs.x + hs.y, 1e-3) * s);
        else t = clamp(-tfx_distanceAt(ivec2(p)) / max(ta.p[10].w, 1e-3), 0.0, 1.0) / s;
        vec4 g = texture(TA_TEX2, ta_lutUv(t - ta.p[2].z));
        c = g.rgb;
        fillA = g.a;
    }
    // the surface normal: the bevel's slope (over the soften width) and a surface texture's
    vec3 n = vec3(0.0, 0.0, 1.0);
    bool lit = bevel || (texKind > 0 && texAmt > 0.0);
    if (lit) {
        vec2 g = vec2(0.0);
        if (bevel) {
            vec2 ex = vec2(soften, 0.0), ey = vec2(0.0, soften);
            g += vec2(tfx_heightAt(p + ex, size, profile, dirSign) - tfx_heightAt(p - ex, size, profile, dirSign),
                      tfx_heightAt(p + ey, size, profile, dirSign) - tfx_heightAt(p - ey, size, profile, dirSign)) / (2.0 * soften) * depth * size * 0.25;
        }
        if (texKind > 0 && texAmt > 0.0) {
            float e = 1.0 / texPx;
            vec2 ts = vec2(tfx_textureHeight(texKind, tq + vec2(e, 0.0), tc) - tfx_textureHeight(texKind, tq - vec2(e, 0.0), tc),
                           tfx_textureHeight(texKind, tq + vec2(0.0, e), tc) - tfx_textureHeight(texKind, tq - vec2(0.0, e), tc)) * 0.5 * texPx;
            g += vec2(texDir.x * ts.x - texDir.y * ts.y, texDir.y * ts.x + texDir.x * ts.y) * texAmt;
        }
        n = normalize(vec3(-g, 1.0));
    }
    if (lit) {
        float sl = dot(n, Ld) - Ld.z;
        float hi = tfx_contour(sl > 0.0 ? sl / max(1.0 - Ld.z, 0.05) : 0.0, gloss);
        float sh = tfx_contour(sl < 0.0 ? -sl / max(Ld.z, 0.05) : 0.0, gloss);
        c = tfx_screen(c, ta.p[7].rgb * hi * ta.p[7].w);
        c *= mix(vec3(1.0), ta.p[8].rgb, clamp(sh * ta.p[8].w, 0.0, 1.0));
    }
    if (ta.p[9].x > 0.5) {
        float lum = dot(c, vec3(0.299, 0.587, 0.114));
        c = mix(c, texture(TA_TEX3, ta_lutUv(lum)).rgb, ta.p[9].y);
    }
    float a = L.a * fillA * ta.p[3].w;
    return vec4(ta_toLinear(clamp(c, 0.0, 1.0)) * a, a);
}

// Material details over the surface: the pattern's marks and colour 2, satin (the difference of
// the blurred silhouette shifted both ways, through its contour), a colour texture, sheen, inner
// glow and the gloss cap. TA_TEX0 the surface, TA_TEX1 the blurred silhouette, TA_TEX2 the input
// (its alpha), TA_TEX3 the satin contour's lookup, TA_RAW0 the distance, TA_RAW1 the bounds.
// p2 = (satin on, opacity, blend 0 normal 1 multiply 2 screen 3 overlay, invert), p3 = (satin
// colour sRGB, unused), p4 = (satin offset px (y down), inner glow on, glow size px), p5 = (glow
// colour sRGB, glow opacity), p6 = (glow noise, gloss cap on, cap opacity, cap position),
// p7 = (cap softness, cap curve, cap direction (y down)), p8 = (sheen on, amount, position,
// width), p9 = (sheen colour sRGB, colour texture kind (0 none)), p10 = (texture amount, feature
// px, blend 0 overlay 1 soft light 2 screen 3 multiply, pattern kind), p11 = (pattern opacity,
// shade (unused: the marks take the lit surface), seed, texture seed), p12 = the pattern's
// motif map (row-major), p13 = (pattern colour sRGB, offset x), p14 = (pattern colour 2 sRGB,
// offset y), p15 = (texture direction (y down))
vec4 tfx_materialDetail() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = ta_uv();
    vec4 S = texture(TA_TEX0, uv);
    if (S.a <= 0.0) return vec4(0.0);
    vec3 c = ta_toSrgb(ta_unpremul(S));
    vec4 b = fx_layerBox();
    vec2 bc = 0.5 * (b.xy + b.zw), hs = max(0.5 * (b.zw - b.xy), vec2(0.5));
    int patKind = int(ta.p[10].w + 0.5);
    if (patKind > 0) {
        vec4 m = ta.p[12];
        vec2 motif = vec2(dot(m.xy, p), dot(m.zw, p)) + vec2(ta.p[13].w, ta.p[14].w);
        vec4 pat = patternSampleD(motif, vec2(m.x, m.z), vec2(m.y, m.w), patKind, ta.p[11].z);
        c = mix(c, ta.p[13].rgb, pat.x * ta.p[11].x);
        c = mix(c, ta.p[14].rgb, pat.y * ta.p[11].x);
    }
    if (ta.p[2].x > 0.5) {
        vec2 off = ta.p[4].xy / TA_SIZE;
        float s1 = texture(TA_TEX1, uv + off).a, s2 = texture(TA_TEX1, uv - off).a;
        float v = texture(TA_TEX3, ta_lutUv(abs(s1 - s2))).r;
        if (ta.p[2].w > 0.5) v = 1.0 - v;
        vec3 sc = ta.p[3].rgb;
        int blend = int(ta.p[2].z + 0.5);
        vec3 bl = blend == 1 ? c * sc : blend == 2 ? tfx_screen(c, sc) : blend == 3 ? mix(2.0 * c * sc, 1.0 - 2.0 * (1.0 - c) * (1.0 - sc), step(0.5, c)) : sc;
        c = mix(c, bl, clamp(v * ta.p[2].y, 0.0, 1.0));
    }
    int texKind = int(ta.p[9].w + 0.5);
    float texAmt = ta.p[10].x;
    float texPx = max(ta.p[10].y, 0.5);
    vec2 texDir = ta.p[15].xy;
    vec2 tq = vec2(dot(p, texDir), dot(p, vec2(-texDir.y, texDir.x))) / texPx + ta.p[11].w;
    vec2 tc = vec2(dot(bc, texDir), dot(bc, vec2(-texDir.y, texDir.x))) / texPx + ta.p[11].w;
    float grain = 0.5;
    if (texKind > 0 && texAmt > 0.0) {
        float v = tfx_textureHeight(texKind, tq, tc);
        grain = v;
        int blend = int(ta.p[10].z + 0.5);
        vec3 r;
        if (blend == 1) r = (1.0 - 2.0 * v) * c * c + 2.0 * v * c;
        else if (blend == 2) { float s = 2.0 * max(v - 0.5, 0.0); r = 1.0 - (1.0 - c) * (1.0 - s); }
        else if (blend == 3) r = c * (1.0 - 2.0 * max(0.5 - v, 0.0));
        else r = mix(2.0 * c * v, 1.0 - 2.0 * (1.0 - c) * (1.0 - v), step(0.5, c));
        c = mix(c, clamp(r, 0.0, 1.0), texAmt);
    }
    if (ta.p[8].x > 0.5) {
        float u = dot((p - bc) / hs, texDir);
        float sheen = exp(-pow((u - ta.p[8].z * 1.2) / max(ta.p[8].w, 0.01), 2.0)) * (0.5 + grain);
        c += ta.p[9].rgb * sheen * ta.p[8].y * (0.4 + 0.6 * c);
    }
    if (ta.p[4].z > 0.5) {
        float d = tfx_distanceAt(ivec2(p));
        float t = clamp(max(-d, 0.0) / max(ta.p[4].w, 0.5), 0.0, 1.0);
        float gi = (1.0 - t) * (1.0 - t);
        gi *= mix(1.0, hash12(p + 13.7) * 2.0, ta.p[6].x);
        c = tfx_screen(c, ta.p[5].rgb * clamp(gi * ta.p[5].w, 0.0, 1.0));
    }
    if (ta.p[6].y > 0.5) {
        vec2 dn = ta.p[7].zw, ac = vec2(-dn.y, dn.x);
        float ed = abs(dn.x) * hs.x + abs(dn.y) * hs.y;
        float ea = abs(ac.x) * hs.x + abs(ac.y) * hs.y;
        float yf = (dot(p - bc, dn) + ed) / (2.0 * ed);
        float u = dot(p - bc, ac) / ea;
        float edge = ta.p[6].w + ta.p[7].y * 0.25 * (1.0 - u * u);
        float sw = max(ta.p[7].x, 0.002) * 0.5;
        float cap = 1.0 - smoothstep(edge - sw, edge + sw, yf);
        cap *= mix(1.0, 0.55, clamp(yf / max(edge, 1e-3), 0.0, 1.0));
        c = mix(c, vec3(1.0), clamp(cap * ta.p[6].z, 0.0, 1.0));
    }
    return vec4(ta_toLinear(clamp(c, 0.0, 1.0)) * S.a, S.a);
}

#endif // TITLEART_FX_MATERIAL_GLSL
