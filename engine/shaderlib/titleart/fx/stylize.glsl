// S83's stylize and colour effects (§6.6.9 Halftone, §6.6.14 Grain, §6.6.20 Scanlines, §6.6.21
// Pixelate, §6.6.27 Holographic Foil). Halftone and Grain are ported from Pachislop
// src/effects/a/stylize.ts (halftone) and finish.ts (grain) by SafEight, used with the author's
// permission (Halftone's square, diamond, line and cross dots are S83's); Scanlines' line term is
// the HTML UI scanlines.frag's (shaders.com kit, MIT, see effects/LICENSE.shaders-com.txt);
// Pixelate, Holographic Foil are original. Pixelate's Bayer thresholds are the HTML UI's
// (effects/lib/jfx_tone.glsl); its OKLab conversion is the one of effects/lib/jfx_color.glsl
// (that file declares the HTML UI's frame block and outputs, so it is not included here);
// Holographic Foil's film is the PBR iridescence (includes/iridescence.glsl).

#ifndef TITLEART_FX_STYLIZE_GLSL
#define TITLEART_FX_STYLIZE_GLSL

#include "effects/lib/jfx_tone.glsl"
#ifndef M_PI
#define M_PI 3.14159265358979
#endif
#include "includes/iridescence.glsl"

// ---- Halftone ----

// an antialiased disc of radius rad (cells) at distance d (cells), px render px per cell; tiny
// discs fade by their area
float tfx_disc(float rad, float d, float px) {
    return clamp((rad - d) * px + 0.5, 0.0, 1.0) * min(1.0, 2.0 * rad * px);
}

// the coverage of one cell's dot for ink amount v (0..2, an area fraction past 1 for round dots,
// Pachislop's), f the position in the cell (-0.5..0.5), px render px per cell
float tfx_halftoneDot(int shape, vec2 f, float v, float px) {
    if (shape == 1) {
        // square: half side so the area is v
        float h = 0.5 * sqrt(clamp(v, 0.0, 1.0));
        vec2 e = abs(f) - h;
        return clamp(-max(e.x, e.y) * px + 0.5, 0.0, 1.0) * min(1.0, 2.0 * h * px);
    }
    if (shape == 2) {
        // diamond: |x| + |y| <= r, area 2 r^2
        float r = sqrt(clamp(v, 0.0, 2.0) * 0.5);
        return clamp((r - abs(f.x) - abs(f.y)) * 0.7071 * px + 0.5, 0.0, 1.0) * min(1.0, 2.0 * r * px);
    }
    if (shape == 3) {
        // line screen: a stripe along the cell's x, its width the area
        float h = 0.5 * clamp(v, 0.0, 1.0);
        return clamp((h - abs(f.y)) * px + 0.5, 0.0, 1.0);
    }
    if (shape == 4) {
        // cross: two bars of width w, area 2w - w^2
        float w = 1.0 - sqrt(max(1.0 - clamp(v, 0.0, 1.0), 0.0));
        float h = 0.5 * w;
        return clamp((h - min(abs(f.x), abs(f.y))) * px + 0.5, 0.0, 1.0);
    }
    float radius = sqrt(clamp(v, 0.0, 2.0)) * 0.7071;
    return clamp((radius - length(f)) * px + 0.5, 0.0, 1.0);
}

// Halftone: an AM screen inside the shape, its dot size from a ramp across the layer box, the
// layer's darkness or uniform, blended over the layer's colour (Photoshop's way, sRGB); Dissolve
// breaks the layer itself into the screen toward its soft parts. TA_TEX0 L, TA_TEX1 the softened
// alpha (Dissolve), TA_RAW1 the bounds (ramp). p2 = (cell canvas px, screen angle rad, dot scale,
// amount), p3 = (dot colour sRGB, source 0 ramp 1 darkness 2 uniform 3 dissolve),
// p4 = (ramp direction (y down), blend mode, dot shape), p5.xy = the canvas centre (target px)
vec4 tfx_halftone() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    if (L.a < 1e-5) return vec4(0.0);
    float size = ta.p[2].x, ang = ta.p[2].y, dotScale = ta.p[2].z, amount = ta.p[2].w;
    int source = int(ta.p[3].w + 0.5), mode = int(ta.p[4].z + 0.5), shape = int(ta.p[4].w + 0.5);
    vec2 centre = ta.p[5].xy;
    // Pachislop's comp px (y up) from the canvas centre
    vec2 d = tfx_canvasPx(gl_FragCoord.xy, centre);
    vec2 pc = vec2(d.x, -d.y);
    float cs = cos(ang), sn = sin(ang);
    mat2 rot = mat2(cs, sn, -sn, cs);
    vec2 r = rot * pc / size;
    vec2 cell = floor(r);
    vec2 f = r - cell - 0.5;
    float px = size * TA_PX;
    if (source == 3) {
        float dens = clamp(1.7 * texture(TA_TEX1, uv).a - 0.7, 0.0, 1.0);
        float scr = dens <= 0.5 ? tfx_disc(sqrt(dens / 3.14159265), length(f), px)
                                : 1.0 - tfx_disc(sqrt((1.0 - dens) / 3.14159265), length(fract(r + 0.5) - 0.5), px);
        return L * scr;
    }
    float v;
    if (source == 0) {
        v = clamp(fx_boxPos(ta.p[4].xy), 0.0, 1.0);
    } else if (source == 1) {
        vec2 offUp = transpose(rot) * ((cell + 0.5) * size) * TA_PX;
        vec4 s = texture(TA_TEX0, (centre + vec2(offUp.x, -offUp.y)) / TA_SIZE);
        if (s.a < 0.05) s = L;
        v = 1.0 - ta_luma(tfx_srgbOf(s));
    } else {
        v = 1.0;
    }
    float cov = tfx_halftoneDot(shape, f, v * dotScale, px);
    vec3 b = tfx_srgbOf(L);
    vec3 res = ta_toLinear(mix(b, ta_blend(mode, b, ta.p[3].rgb), cov * amount));
    return vec4(res * L.a, L.a);
}

// ---- Grain ----

float tfx_vnoiseSeeded(vec2 p, float s) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i + s), b = hash12(i + vec2(1.0, 0.0) + s), c = hash12(i + vec2(0.0, 1.0) + s), d = hash12(i + 1.0 + s);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
// two octaves: clumpy like silver grain, not a regular pixel grid
float tfx_grainAt(vec2 q, float s) {
    return 0.65 * tfx_vnoiseSeeded(q, s) + 0.35 * tfx_vnoiseSeeded(q * 2.03 + 5.2, s + 9.0) - 0.5;
}

// Grain: film grain in canvas px (the same at every scale), strongest in the midtones;
// transparency untouched. TA_TEX0 L. p2 = (amount, size canvas px, seed, colour 0/1),
// p3.xy = the canvas centre (target px)
vec4 tfx_grain() {
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return L;
    vec2 d = tfx_canvasPx(gl_FragCoord.xy, ta.p[3].xy);
    vec2 q = vec2(d.x, -d.y) / ta.p[2].y;
    float s = ta.p[2].z;
    vec3 n = ta.p[2].w > 0.5 ? vec3(tfx_grainAt(q, s), tfx_grainAt(q, s + 17.0), tfx_grainAt(q, s + 31.0)) : vec3(tfx_grainAt(q, s));
    vec3 lin = ta_unpremul(L);
    float hdr = max(max(lin.r, max(lin.g, lin.b)), 1.0);
    vec3 c = ta_toSrgb(lin / hdr);
    float l = ta_luma(c);
    c += n * ta.p[2].x * (0.3 + 2.8 * l * (1.0 - l));
    return vec4(ta_toLinear(clamp(c, 0.0, 1.0)) * hdr * L.a, L.a);
}

// ---- Scanlines ----

// Scanlines: lines (the HTML UI's term: 1 - intensity x a cosine wave whose dark part Thickness
// stretches), CRT (with an aperture grille of RGB stripes), interlace (hard alternate lines) or
// LCD (RGB sub-cells), on linear premultiplied colour, brightened to compensate. Positions in
// canvas px across the line direction. TA_TEX0 L. p2 = (spacing canvas px, thickness, intensity,
// style 0 lines 1 crt 2 interlace 3 lcd), p3 = (line normal (y down), offset canvas px, mask
// strength), p4 = (brightness, flicker, seed), p5.xy = the canvas centre (target px)
vec4 tfx_scanlines() {
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return L;
    float spacing = max(ta.p[2].x, 0.25), thickness = ta.p[2].y, intensity = ta.p[2].z;
    int style = int(ta.p[2].w + 0.5);
    vec2 nrm = ta.p[3].xy;
    vec2 c = tfx_canvasPx(gl_FragCoord.xy, ta.p[5].xy);
    float y = dot(c, nrm) + ta.p[3].z;
    float u = dot(c, vec2(-nrm.y, nrm.x));
    float x = abs(fract(y / spacing) - 0.5);
    float halfT = clamp(thickness, 0.05, 0.95) * 0.5;
    float band;
    if (style == 2) {
        // interlace: every other line, hard edged (antialiased over a pixel)
        float aa = 0.5 / max(spacing * TA_PX, 1.0);
        float ph = fract(y / (2.0 * spacing));
        band = smoothstep(0.5 - aa, 0.5 + aa, ph) * (1.0 - smoothstep(1.0 - aa, 1.0, ph));
    } else {
        float warped = x < halfT ? x * 0.25 / halfT : 0.25 + (x - halfT) * 0.25 / max(0.5 - halfT, 1e-4);
        band = 0.5 + 0.5 * cos(2.0 * TA_PI * warped);
    }
    vec3 mask = vec3(1.0);
    float m = ta.p[3].w;
    if (style == 1 || style == 3) {
        // the stripe (CRT: the aperture grille along the lines; LCD: a sub-cell) a pixel falls in
        float k = fract(u / spacing) * 3.0;
        vec3 stripe = vec3(step(k, 1.0), step(1.0, k) * step(k, 2.0), step(2.0, k));
        mask = mix(vec3(1.0), stripe * 3.0 * 0.85, m);
        if (style == 3) {
            // the LCD cell's gap along the line direction
            band = max(band, smoothstep(0.42, 0.5, abs(fract(u / spacing * 3.0) - 0.5)) * m);
        }
    }
    float shade = 1.0 - intensity * band;
    float flicker = ta.p[4].y;
    if (flicker > 0.0) shade *= 1.0 - flicker * 0.25 * hash12(vec2(ta.p[4].z, 0.37));
    vec3 rgb = ta_unpremul(L) * shade * mask * ta.p[4].x;
    return vec4(rgb * L.a, L.a);
}

// ---- Pixelate ----

vec3 tfx_oklab(vec3 rgb) {
    float l = rgb.r * 0.4122214708 + rgb.g * 0.5363325363 + rgb.b * 0.0514459929;
    float m = rgb.r * 0.2119034982 + (rgb.g * 0.6806995451 + rgb.b * 0.1073969566);
    float s = rgb.r * 0.0883024619 + rgb.g * 0.2817188376 + rgb.b * 0.6299787005;
    float l_ = sign(l) * pow(abs(l), 1.0 / 3.0);
    float m_ = sign(m) * pow(abs(m), 1.0 / 3.0);
    float s_ = sign(s) * pow(abs(s), 1.0 / 3.0);
    return vec3(l_ * 0.2104542553 + m_ * 0.7936177850 - s_ * 0.0040720468,
                l_ * 1.9779984951 - m_ * 2.4285922050 + s_ * 0.4505937099,
                l_ * 0.0259040371 + m_ * 0.7827717662 - s_ * 0.8086757660);
}

// One cell: the coverage-weighted (premultiplied) mean of taps x taps samples, then posterized
// per channel or snapped to the nearest palette colour in OKLab, after an ordered dither of the
// cell's Bayer threshold; the alpha hard (thresholded) or soft. Output: one texel per cell.
// TA_TEX0 the source (or a downsampled copy, sampled by uv of the full target), TA_TEX2 the
// palette image (texelFetch, row-major, up to 256 colours). p2 = (cell target px, grid origin
// target px, taps per axis), p3 = (full target size, levels, dither 0 none 1 bayer2 2 bayer4
// 3 bayer8), p4 = (dither amount, alpha 0 hard 1 soft, alpha threshold, palette colours),
// p5.x = the palette image's width
vec4 tfx_pixelDown() {
    vec2 cellIdx = floor(gl_FragCoord.xy);
    float cell = ta.p[2].x;
    vec2 origin = ta.p[2].yz;
    int taps = int(ta.p[2].w + 0.5);
    vec2 full = ta.p[3].xy;
    vec4 acc = vec4(0.0);
    for (int y = 0; y < 16; y++) {
        if (y >= taps) break;
        for (int x = 0; x < 16; x++) {
            if (x >= taps) break;
            vec2 q = origin + (cellIdx + (vec2(x, y) + 0.5) / float(taps)) * cell;
            acc += textureLod(TA_TEX0, q / full, 0.0);
        }
    }
    vec4 c = acc / float(max(taps * taps, 1));
    if (c.a < 1e-4) return vec4(0.0);
    vec3 s = clamp(ta_toSrgb(c.rgb / c.a), 0.0, 1.0);
    int dither = int(ta.p[3].w + 0.5);
    float levels = ta.p[3].z;
    int paletteCount = int(ta.p[4].w + 0.5);
    float th = dither == 1 ? jfx_bayer2(cellIdx) : dither == 2 ? jfx_bayer4(cellIdx) : dither == 3 ? jfx_bayer8(cellIdx) : 0.5;
    float qstep = paletteCount > 0 ? 1.0 / max(float(paletteCount) - 1.0, 1.0) : levels > 1.0 ? 1.0 / (levels - 1.0) : 0.0;
    if (dither > 0) s = clamp(s + (th - 0.5) * ta.p[4].x * qstep, 0.0, 1.0);
    if (paletteCount > 0) {
        int width = max(int(ta.p[5].x + 0.5), 1);
        vec3 lab = tfx_oklab(ta_toLinear(s));
        float best = 1e9;
        vec3 pick = s;
        for (int i = 0; i < 256; i++) {
            if (i >= paletteCount) break;
            vec3 e = texelFetch(TA_TEX2, ivec2(i - (i / width) * width, i / width), 0).rgb;
            vec3 dl = tfx_oklab(ta_toLinear(e)) - lab;
            float dd = dot(dl, dl);
            if (dd < best) {
                best = dd;
                pick = e;
            }
        }
        s = pick;
    } else if (levels > 1.0) {
        s = floor(s * (levels - 1.0) + 0.5) / (levels - 1.0);
    }
    float a = ta.p[4].y > 0.5 ? c.a : step(ta.p[4].z, c.a);
    return vec4(ta_toLinear(s) * a, a);
}

// The cells drawn back at full size as squares, discs, diamonds or LED dots with a gap.
// TA_TEX1 the cells (texelFetch). p2 = (cell target px, grid origin target px, shape 0 square
// 1 round 2 diamond 3 led), p3 = (gap 0..0.9, the cells' grid size)
vec4 tfx_pixelUp() {
    float cell = ta.p[2].x;
    vec2 q = (gl_FragCoord.xy - ta.p[2].yz) / cell;
    vec2 idx = floor(q);
    ivec2 grid = ivec2(ta.p[3].yz + 0.5);
    ivec2 i = ivec2(idx);
    if (i.x < 0 || i.y < 0 || i.x >= grid.x || i.y >= grid.y) return vec4(0.0);
    vec4 c = texelFetch(TA_TEX1, i, 0);
    int shape = int(ta.p[2].w + 0.5);
    float gap = ta.p[3].x;
    vec2 f = q - idx - 0.5;
    float h = 0.5 * (1.0 - gap);
    float aa = 0.5 / max(cell, 1.0);
    float cov;
    if (shape == 1 || shape == 3) cov = 1.0 - smoothstep(h - aa, h + aa, length(f));
    else if (shape == 2) cov = 1.0 - smoothstep(h - aa, h + aa, abs(f.x) + abs(f.y));
    else cov = gap > 0.0 ? 1.0 - smoothstep(h - aa, h + aa, max(abs(f.x), abs(f.y))) : 1.0;
    if (shape == 3) {
        // an LED: brighter in its centre, a dim rim
        float r = clamp(length(f) / max(h, 1e-3), 0.0, 1.0);
        c.rgb *= 0.55 + 0.75 * (1.0 - r * r);
    }
    return c * cov;
}

// ---- Holographic Foil ----

// Holographic Foil: a thin-film colour (the PBR iridescence) seen at an angle set by the tilt,
// the position on the layer, a bevel from the distance field and a micro pattern (diffraction
// grating, glitter flakes, star sparkles), blended over the layer. TA_TEX0 L, TA_RAW0 the
// content's distance (relief). p2 = (amount, tilt xy, film thickness nm), p3 = (film ior, relief,
// pattern 0 none 1 diffraction 2 glitter 3 stars, pattern scale canvas px), p4 = (seed, blend
// mode, relief width px, canvas half diagonal canvas px), p5.xy = the canvas centre (target px)
vec4 tfx_holographic() {
    vec2 p = gl_FragCoord.xy;
    vec4 L = texture(TA_TEX0, ta_uv());
    if (L.a < 1e-5) return vec4(0.0);
    float amount = ta.p[2].x;
    vec2 tilt = ta.p[2].yz;
    float thickness = ta.p[2].w, ior = ta.p[3].x, relief = ta.p[3].y;
    int pattern = int(ta.p[3].z + 0.5);
    float scale = max(ta.p[3].w, 0.5), seed = ta.p[4].x;
    int mode = int(ta.p[4].y + 0.5);
    vec2 c = tfx_canvasPx(p, ta.p[5].xy);
    // the surface normal: flat, bevelled toward the edge by the relief
    vec3 N = vec3(0.0, 0.0, 1.0);
    if (relief > 0.0) {
        float d = tfx_distanceAt(ivec2(p));
        float w = max(ta.p[4].z, 1.0);
        vec2 n2 = tfx_distanceNormal(p) * (1.0 - smoothstep(0.0, w, -d)) * relief;
        float wob = relief * 0.35 * (vnoise(c / (8.0 * scale) + seed) - 0.5);
        N = normalize(vec3(n2 + vec2(wob, -wob), 1.0));
    }
    // micro pattern: a per-flake or per-groove tilt of the normal and a film-thickness shift
    float shift = 0.0, sparkle = 0.0;
    if (pattern == 1) {
        shift = fract(dot(c, vec2(0.8, 0.6)) / scale) * 0.6;
    } else if (pattern == 2) {
        vec3 w = tfx_worley(c / scale, seed);
        vec2 h = hash22(vec2(w.z * 113.1, seed)) * 2.0 - 1.0;
        N = normalize(N + vec3(h * 0.45, 0.0));
        sparkle = pow(max(1.0 - w.x * 2.2, 0.0), 6.0) * step(0.8, w.z);
    } else if (pattern == 3) {
        vec2 q = c / (scale * 3.0);
        vec2 i = floor(q);
        vec2 f = q - i - 0.5 - (hash22(i + seed) - 0.5) * 0.5;
        float star = max(1.0 - abs(f.x * f.y) * 90.0 - length(f) * 3.0, 0.0);
        sparkle = star * star * step(0.55, hash12(i + seed * 2.1));
        shift = hash12(i + seed * 7.7) * 0.4;
    }
    // the view: the tilt plus where on the canvas the point lies (a sheet seen under one light)
    vec3 V = normalize(vec3(tilt * 0.9 + c / max(ta.p[4].w, 1.0) * 0.5, 1.0));
    float cosT = clamp(abs(dot(N, V)), 0.0, 1.0);
    vec3 film = iridescenceFresnel(cosT, ior, thickness * (1.0 + shift), vec3(0.04));
    // normalised to its brightest channel: the foil's hue, at full strength
    film = clamp(film / max(max(film.r, max(film.g, film.b)), 1e-4), 0.0, 1.0);
    vec3 foil = ta_toSrgb(film);
    vec3 b = tfx_srgbOf(L);
    vec3 r = ta_toLinear(mix(b, ta_blend(mode, b, foil), amount)) + ta_toLinear(vec3(sparkle)) * amount;
    return vec4(r * L.a, L.a);
}

#endif // TITLEART_FX_STYLIZE_GLSL
