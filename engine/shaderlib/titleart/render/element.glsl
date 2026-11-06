// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/layer2d/shaders.ts (FILL_GLSL, ELEMENT_FRAG) and
// src/engine/patterns.ts (the pattern uniforms) by SafEight, used with the author's permission.
// The procedural textures' hashes are Dave Hoskins' "Hash without Sine" (MIT, noise.glsl).

// One element of a 2D title (a face group or a rim) shaded over its coverage into the stack
// (§6.7 pass 10), in Pachislop's order: pattern sample and relief -> the normal from the bevel
// slope, the pattern relief and a surface texture -> refraction -> fill (8 kinds, an image's own
// picture, line / char spans with an analytic seam) -> glass -> reflection map -> bevel light
// (highlight screen and shadow multiply through the gloss contour) -> finish (gradient map of
// the lit luma) -> pattern marks -> satin -> colour texture -> sheen -> inner glow -> gloss cap
// -> alpha = coverage x fill alpha x opacity (glass raises it), written linear premultiplied.
// The colour maths runs on sRGB-encoded values, like Photoshop. The frame is y-up (common.glsl):
// gl_FragCoord is Pachislop's frame pixel. The knockout pass (uPunch) writes the coverage only,
// blended as dst x (1 - a).
//
// Slots (render/Title2DPass.cpp binds them): TA_TEX0 coverage, TA_TEX1 satin blur, TA_TEX2 the
// custom pattern tile, TA_TEX3 the fill image, TA_TEX4 the reflection panorama, TA_TEX5 the LUTs
// (rows: 0 fill gradient, 1 finish gradient, 2 gloss curve, 3 satin contour), TA_TEX6 the
// picture; TA_RAW0 signed distance, TA_RAW1 height, TA_RAW2 nearest glyph, TA_RAW3 the glyph
// boxes (RGBA32F N x 2), TA_RAW4 the shape-burst depth (1 x 1).

#define TA_FILL_IMAGE TA_TEX3
#define TA_REFLECTION TA_TEX4
#define TA_LUTS TA_TEX5
#define TA_PICTURE TA_TEX6
#define TA_LUT_ROWS 4.0
#define TA_BURST_ENC (ta.p[35])

#define toLinear taToLinear
#define toSrgb taToSrgb

// the uniforms of Pachislop's element program, as members of the block (Title2DPass.cpp's
// ElementParams writes them)
#define uFillKind int(ta.p[2].x + 0.5)
#define uFillScale (ta.p[2].y)
#define uFillOffset (ta.p[2].z)
#define uFillPx (ta.p[2].w)
#define uFillColor (ta.p[3].rgb)
#define uImgOn (ta.p[3].w > 0.5)
#define uFillDir (ta.p[4].xy)
#define uImgAspect (ta.p[4].z)
#define uImgScale (ta.p[4].w)
#define uImgOff (ta.p[5].xy)
#define uImgRot (ta.p[5].zw)
#define uImgTile (ta.p[6].x > 0.5)
#define uPicture (ta.p[6].y > 0.5)
#define uPictureLinear (ta.p[6].z > 0.5)
#define uHasSdf (ta.p[6].w > 0.5)
#define uBmin (ta.p[7].xy)
#define uBmax (ta.p[7].zw)
#define uLight (ta.p[8].xyz)
#define uSpan int(ta.p[8].w + 0.5)
#define uFrameMin (ta.p[9].xy)
#define uM (ta.p[9].z)
#define uSpanGrow (ta.p[9].w)
#define uElemOff (ta.p[10].xy)
#define uBurstPx (ta.p[10].z)
#define uBevel (ta.p[10].w > 0.5)
#define uBevelK (ta.p[11].x)
#define uGloss int(ta.p[11].y + 0.5)
#define uHiOp (ta.p[11].z)
#define uShOp (ta.p[11].w)
#define uHiColor (ta.p[12].rgb)
#define uRefrPx (ta.p[12].w)
#define uShColor (ta.p[13].rgb)
#define uGlass (ta.p[13].w > 0.5)
#define uGlassColor (ta.p[14].rgb)
#define uGlassTint (ta.p[14].w)
#define uGlassOp (ta.p[15].x)
#define uGlassEdge (ta.p[15].y)
#define uGlassCaustic (ta.p[15].z)
#define uGlassCw (ta.p[15].w)
#define uRefl (ta.p[16].x > 0.5)
#define uReflAmt (ta.p[16].y)
#define uReflRot (ta.p[16].z)
#define uReflTint (ta.p[16].w)
#define uReflM mat2(ta.p[17].xy, ta.p[17].zw)
#define uSatin (ta.p[18].x > 0.5)
#define uSatinOff (ta.p[18].yz)
#define uSatinOp (ta.p[18].w)
#define uSatinColor (ta.p[19].rgb)
#define uSatinBlend int(ta.p[19].w + 0.5)
#define uSatinInvert (ta.p[20].x > 0.5)
#define uFinish (ta.p[20].y > 0.5)
#define uFinishAmt (ta.p[20].z)
#define uTex int(ta.p[20].w + 0.5)
#define uTexAmt (ta.p[21].x)
#define uTexPx (ta.p[21].y)
#define uTexBlend int(ta.p[21].z + 0.5)
#define uTexSurface (ta.p[21].w > 0.5)
#define uTexSeed (ta.p[22].xy)
#define uTexRot (ta.p[22].zw)
#define uSheen (ta.p[23].x > 0.5)
#define uSheenAmt (ta.p[23].y)
#define uSheenPos (ta.p[23].z)
#define uSheenWidth (ta.p[23].w)
#define uSheenColor (ta.p[24].rgb)
#define uGlow (ta.p[24].w > 0.5)
#define uGlowColor (ta.p[25].rgb)
#define uGlowPx (ta.p[25].w)
#define uGlowOp (ta.p[26].x)
#define uGlowNoise (ta.p[26].y)
#define uPxOff (ta.p[26].zw)
#define uCap (ta.p[27].x > 0.5)
#define uCapOp (ta.p[27].y)
#define uCapPos (ta.p[27].z)
#define uCapSoft (ta.p[27].w)
#define uCapCurve (ta.p[28].x)
#define uCapDir (ta.p[28].yz)
#define uOpacity (ta.p[28].w)
#define uPunch (ta.p[29].x > 0.5)
#define uPatKind int(ta.p[30].x + 0.5)
#define uPatSeed (ta.p[30].y)
#define uPatOpacity (ta.p[30].z)
#define uPatShade (ta.p[30].w)
#define uPatXform (ta.p[31])
#define uPatOffset (ta.p[32].xy)
#define uPatRelief (ta.p[32].z)
#define uPatColor (ta.p[33].rgb)
#define uPatColor2 (ta.p[34].rgb)

vec2 patternCoord(vec2 pos) { return vec2(dot(uPatXform.xy, pos), dot(uPatXform.zw, pos)) + uPatOffset; }

// ---- fills (Pachislop's FILL_GLSL): every fill kind over a span box (bmin..bmax, the units of
// p) -> sRGB colour + straight alpha; `burst` the shape-burst coordinate (kind 6); kind 7 an image
// cover-fitted over the span box, textureGrad with uFillPx's footprint so tiles have no seams

// row `row` of the LUT texture (TA_LUT_ROWS rows of 256): sRGB colour, straight alpha
vec4 rampAt4(float row, float t) {
  return texture(TA_LUTS, vec2(clamp(t, 0.0, 1.0) * (255.0 / 256.0) + 0.5 / 256.0, (row + 0.5) / TA_LUT_ROWS));
}
vec3 rampAt(float row, float t) { return rampAt4(row, t).rgb; }
vec4 imageFill(vec2 p, vec2 bmin, vec2 bmax) {
  if (!uImgOn) return vec4(uFillColor, 1.0);
  vec2 bs = max(bmax - bmin, vec2(1e-3));
  vec2 v = p - 0.5 * (bmin + bmax) - uImgOff * bs;
  // Undo the clockwise-on-screen rotation (a counter-clockwise turn in y-up space).
  vec2 r = vec2(uImgRot.x * v.x - uImgRot.y * v.y, uImgRot.y * v.x + uImgRot.x * v.y);
  // Cover fit: the image's smaller relative side spans the box at scale 1.
  float w = max(bs.x, bs.y * uImgAspect) * max(uImgScale, 1e-3);
  vec2 size = vec2(w, w / uImgAspect);
  vec2 uv = r / size + 0.5;
  vec2 st = uImgTile ? fract(uv) : clamp(uv, 0.0, 1.0);
  vec2 dx = uImgRot * uFillPx / size;
  vec2 dy = vec2(-uImgRot.y, uImgRot.x) * uFillPx / size;
  // Texture rows run top → bottom: flip v.
  return textureGrad(TA_FILL_IMAGE, vec2(st.x, 1.0 - st.y), vec2(dx.x, -dx.y), vec2(dy.x, -dy.y));
}
vec4 fillColor(vec2 p, vec2 bmin, vec2 bmax, float burst) {
  if (uFillKind == 0) return vec4(uFillColor, 1.0);
  if (uFillKind == 7) return imageFill(p, bmin, bmax);
  vec2 c = 0.5 * (bmin + bmax);
  vec2 hs = 0.5 * (bmax - bmin);
  vec2 v = p - c;
  float s = max(uFillScale, 1e-3);
  float ext = max(abs(uFillDir.x) * hs.x + abs(uFillDir.y) * hs.y, 1e-3);
  float t;
  if (uFillKind == 1) {
    t = dot(v, uFillDir) / (2.0 * ext * s) + 0.5;
  } else if (uFillKind == 2) {
    t = length(v) / (max(length(hs), 1e-3) * s);
  } else if (uFillKind == 3) {
    // Clockwise on screen (y-up: negative cross) from the gradient direction, 0..1 per turn.
    float a = atan(-(uFillDir.x * v.y - uFillDir.y * v.x), dot(uFillDir, v));
    t = (a < 0.0 ? a + 6.2831853 : a) / (6.2831853 * s);
  } else if (uFillKind == 4) {
    t = abs(dot(v, uFillDir)) / (ext * s);
  } else if (uFillKind == 5) {
    t = (abs(v.x) + abs(v.y)) / (max(hs.x + hs.y, 1e-3) * s);
  } else {
    t = burst / s;
  }
  return rampAt4(0.0, t - uFillOffset);
}

// ---- the element ----
// Gloss contours: remap highlight/shadow strength (0..1 → 0..1, contour(0) = 0 so flat
// areas stay neutral). Ring-type contours re-brighten/darken mid slopes → metallic bands.
float contour(float x) {
  x = clamp(x, 0.0, 1.0);
  if (uGloss == 6) return taLut16(TA_LUTS, 2, x);                                                 // custom
  if (uGloss == 1) { float s = sin(x * 2.6); return s * s; }                                  // ring
  if (uGloss == 2) { float s = sin(x * 5.5); return s * s; }                                  // doubleRing
  if (uGloss == 3) return 1.0 - sqrt(max(1.0 - x * x, 0.0));                                  // cove
  if (uGloss == 4) { float f = min(x, 0.9999) * 4.0; return (floor(f) + smoothstep(0.3, 0.7, fract(f))) / 4.0; } // roundedSteps
  if (uGloss == 5) return fract(min(x, 0.9999) * 3.0) * (0.6 + 0.4 * x);                      // sawtooth
  return x;                                                                                     // linear
}

float hFetch(ivec2 q) {
  ivec2 mx = textureSize(TA_RAW1, 0) - 1;
  return taDecodeScalar(texelFetch(TA_RAW1, clamp(q, ivec2(0), mx), 0), TA_RAW_ENC(1));
}

// Procedural textures: 0..1 with 0.5 = neutral.
float sparkle(vec2 q) {
  vec2 cell = floor(q);
  float best = 0.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 c = cell + vec2(x, y);
    vec2 h = hash22(c);
    if (hash12(c + 7.3) > 0.45) continue;
    vec2 d = q - (c + h);
    float size = 0.25 + 0.75 * hash12(c + 3.1);
    float core = exp(-dot(d, d) / (0.012 * size));
    float arms = (exp(-abs(d.y) * 60.0) * max(0.0, 1.0 - abs(d.x) / (0.9 * size))
               + exp(-abs(d.x) * 60.0) * max(0.0, 1.0 - abs(d.y) / (0.9 * size)));
    best = max(best, (core + arms * 0.8) * size);
  }
  return 0.5 + 0.5 * min(best, 1.0);
}

// Hammered: concave dimples; the slope of the nearest dimple (height per feature unit).
vec2 hammeredSlope(vec2 q) {
  vec2 cell = floor(q);
  float dmin = 1e9; vec2 vmin = vec2(0.0);
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 c = cell + vec2(x, y);
    vec2 v = c + 0.15 + 0.7 * hash22(c) - q;
    float d = dot(v, v);
    if (d < dmin) { dmin = d; vmin = v; }
  }
  // The surface falls toward the dimple centre, so the normal tilts toward it.
  return -vmin * 1.4;
}

// Crumpled foil: two octaves of large Voronoi facets, each a tilted plane with a random slope;
// creases (cell borders) are softened over a narrow band so they stay anti-aliased.
vec2 crumpleSlope(vec2 q) {
  vec2 s = vec2(0.0);
  float amp = 1.0;
  for (int o = 0; o < 2; o++) {
    vec2 cell = floor(q);
    float d1 = 1e9, d2 = 1e9;
    vec2 c1 = cell, c2 = cell;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
      vec2 c = cell + vec2(x, y);
      vec2 v = c + 0.1 + 0.8 * hash22(c + float(o) * 17.0) - q;
      float d = dot(v, v);
      if (d < d1) { d2 = d1; c2 = c1; d1 = d; c1 = c; }
      else if (d < d2) { d2 = d; c2 = c; }
    }
    vec2 s1 = hash22(c1 + 31.7) * 2.0 - 1.0, s2 = hash22(c2 + 31.7) * 2.0 - 1.0;
    float w = smoothstep(0.0, 0.05, sqrt(d2) - sqrt(d1));
    s += amp * mix(0.5 * (s1 + s2), s1, w);
    q = q * 2.17 + 5.3;
    amp *= 0.45;
  }
  return s * 0.9;
}

// Liquid metal: smooth domain-warped flow (quintic gradient noise, so the derivative is smooth
// and gloss-contour bands bend into continuous molten swirls).
float liquidH(vec2 q) {
  q *= 0.32;
  vec2 w = vec2(pat_gnoise(q + vec2(1.7, 9.2)), pat_gnoise(q + vec2(8.3, 2.8)));
  vec2 w2 = vec2(pat_gnoise(q * 1.7 + 1.8 * w + vec2(4.1, 7.3)), pat_gnoise(q * 1.7 + 1.8 * w + vec2(1.2, 5.5)));
  return 0.7 * pat_gnoise(q + 1.5 * w2) + 0.3 * pat_gnoise(q * 2.3 + w2 + 6.1);
}

// Element box centre (grid px, see uPxOff) / half size (frame px), set first thing in main.
vec2 gBoxC, gBoxH;

// Texture2D.angle: frame px into the texture's own axes (grain along +x) and slopes back.
vec2 texRotate(vec2 v) { return vec2(uTexRot.x * v.x + uTexRot.y * v.y, -uTexRot.y * v.x + uTexRot.x * v.y); }
vec2 texUnrotate(vec2 v) { return vec2(uTexRot.x * v.x - uTexRot.y * v.y, uTexRot.y * v.x + uTexRot.x * v.y); }

// Metal grain (hairline / spun / flakes): smooth 1D value noise.
float metalN1(float x, float seed) {
  float i = floor(x), f = fract(x);
  f = f * f * (3.0 - 2.0 * f);
  return mix(hash12(vec2(i, seed)), hash12(vec2(i + 1.0, seed)), f);
}
// Flakes: the nearest Voronoi cell of q and the squared distance to its point.
void flakeCell(vec2 q, out vec2 cell, out float best) {
  vec2 g0 = floor(q), f0 = fract(q);
  best = 9.0;
  cell = g0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 c = g0 + vec2(x, y);
    vec2 dv = vec2(x, y) + hash22(c) - f0;
    float dd = dot(dv, dv);
    if (dd < best) { best = dd; cell = c; }
  }
}
// How squarely a flake (a tilted mirror) reflects the sheen light, from Sheen2D.position and the angle, at the viewer.
float flakeFacing(vec2 cell) {
  float la = uSheenPos * 3.1415927 + atan(uTexRot.y, uTexRot.x);
  vec3 L = normalize(vec3(cos(la), sin(la), 1.2));
  vec3 fn = normalize(vec3((hash22(cell + 7.0) - 0.5) * 1.4, 1.0));
  return max(dot(reflect(-L, fn), vec3(0.0, 0.0, 1.0)), 0.0);
}

// Height-like value 0..1 of the noise-type textures (1–6, 10–12), 0.5 = neutral. px: grid px
// (pa, see uPxOff) in the texture's axes; q: feature units.
float textureHeight(vec2 px, vec2 q) {
  if (uTex == 1) return clamp(0.5 + (vnoise(q * 2.0) - 0.5) * 1.3 + (hash12(px + uTexSeed) - 0.5) * 0.35, 0.0, 1.0);
  if (uTex == 2) return sparkle(q * 0.5);
  if (uTex == 3) return clamp(0.5 + (fbm(vec2(q.x * 0.06, q.y * 2.5)) - 0.5) * 1.6, 0.0, 1.0);
  if (uTex == 4) return 0.5 + 0.45 * sin(6.2831853 * 2.5 * fbm(q * 0.35));
  if (uTex == 5) {
    float blot = smoothstep(0.5, 0.72, fbm(q * 0.45));
    float speck = step(0.93, hash12(floor(q * 3.0)));
    float scratch = smoothstep(0.985, 1.0, 1.0 - abs(vnoise(vec2(q.x * 0.12 + q.y * 0.5, q.y * 0.04)) - 0.5) * 2.0);
    return clamp(0.5 - blot * 0.45 - speck * 0.25 + scratch * 0.4, 0.0, 1.0);
  }
  if (uTex == 6) return clamp(0.5 + (fbm(q * 0.25) - 0.5) * 1.6, 0.0, 1.0);
  if (uTex == 10) {
    // Hairline: fine lines across the grain, long slow variation along it.
    float lines = 0.55 * metalN1(q.y, 1.0) + 0.3 * metalN1(q.y * 2.7, 2.0) + 0.15 * hash12(vec2(floor(q.y * 4.0), 3.0));
    float along = vnoise(vec2(q.x / 90.0, q.y * 0.21));
    return mix(lines, lines * along * 1.6, 0.35);
  }
  if (uTex == 11) {
    // Spun: rings around the element centre.
    vec2 dv = (px - texRotate(gBoxC)) / max(uTexPx, 0.5);
    float ring = length(dv);
    float lines = 0.6 * metalN1(ring, 4.0) + 0.4 * metalN1(ring * 3.1, 5.0);
    return lines * (0.75 + 0.5 * vnoise(dv / max(ring, 1e-3) * 3.0 + floor(ring) * 0.13));
  }
  if (uTex == 12) {
    // Flakes / gold leaf: facets lit by how squarely they catch the light.
    vec2 cell;
    float best;
    flakeCell(q, cell, best);
    return 0.3 + 0.7 * flakeFacing(cell) * (0.6 + 0.4 * hash12(cell));
  }
  return 0.5;
}

// Surface slope of the texture at grid px p (height px per px, scaled by amount): the texture's
// height is amount × its feature size, so bumps keep their steepness at any scale.
vec2 textureSlope(vec2 p) {
  vec2 px = texRotate(p);
  float S = max(uTexPx, 0.5);
  vec2 q = px / S + uTexSeed;
  vec2 s;
  if (uTex == 7) s = hammeredSlope(q * 0.5) * 0.5;
  else if (uTex == 8) s = crumpleSlope(q * 0.35) * 0.35 * 2.2;
  else if (uTex == 9) {
    const float e = 0.02;
    s = vec2(liquidH(q + vec2(e, 0.0)) - liquidH(q - vec2(e, 0.0)), liquidH(q + vec2(0.0, e)) - liquidH(q - vec2(0.0, e))) / (2.0 * e) * 2.4;
  } else {
    // Finite differences over one frame pixel.
    vec2 ex = vec2(1.0, 0.0), ey = vec2(0.0, 1.0);
    s = vec2(textureHeight(px + ex, q + ex / S) - textureHeight(px - ex, q - ex / S),
             textureHeight(px + ey, q + ey / S) - textureHeight(px - ey, q - ey / S)) * 0.5 * S;
  }
  return texUnrotate(s) * uTexAmt;
}

// Colour-target texture value 0..1 (0.5 neutral). Relief kinds are shaded by the bevel light
// (hammered keeps its v4 dimple depth: twice its surface slope).
float textureValue(vec2 p) {
  vec2 px = texRotate(p);
  if (uTex >= 7 && uTex <= 9) {
    vec2 g = uTex == 7 ? texUnrotate(hammeredSlope(px / max(uTexPx, 0.5) * 0.5 + uTexSeed * 0.5)) : textureSlope(p) / max(uTexAmt, 1e-3);
    vec3 n = normalize(vec3(-g, 1.0));
    return clamp(0.5 + (dot(n, uLight) - uLight.z) * 1.4, 0.0, 1.0);
  }
  return textureHeight(px, px / max(uTexPx, 0.5) + uTexSeed);
}

// Sheen2D strength at grid px p: a band across the grain (spun: radial lobes; flakes: the facets
// that catch the light), modulated by the metal grain.
float sheenShape(vec2 p) {
  vec2 px = texRotate(p);
  vec2 q = px / max(uTexPx, 0.5) + uTexSeed;
  float grain = uTex >= 10 ? textureHeight(px, q) : 0.5;
  if (uTex == 11) {
    vec2 dv = p - gBoxC;
    float th = atan(dv.y, dv.x) - uSheenPos * 3.1415927 - atan(uTexRot.y, uTexRot.x);
    return pow(abs(cos(th)), 6.0 / max(uSheenWidth * uSheenWidth, 0.01)) * (0.5 + grain);
  }
  if (uTex == 12) {
    vec2 cell;
    float best;
    flakeCell(q, cell, best);
    float g = pow(flakeFacing(cell), 18.0 / max(uSheenWidth, 0.05));
    // A bright facet, and a sparkle point at its centre.
    return g * step(0.4, hash12(cell + 19.0)) * (1.2 + 3.0 * exp(-best * 30.0));
  }
  float u = dot((p - gBoxC) / gBoxH, uTexRot);
  return exp(-pow((u - uSheenPos * 1.2) / uSheenWidth, 2.0)) * (0.5 + grain);
}

vec3 blendTex(vec3 b, float v) {
  if (uTexBlend == 1) return (1.0 - 2.0 * v) * b * b + 2.0 * v * b;                      // soft light
  if (uTexBlend == 2) { float s = 2.0 * max(v - 0.5, 0.0); return 1.0 - (1.0 - b) * (1.0 - s); } // screen (above-mid only)
  if (uTexBlend == 3) { float m = 1.0 - 2.0 * max(0.5 - v, 0.0); return b * m; }           // multiply (below-mid only)
  vec3 lo = 2.0 * b * v, hi = 1.0 - 2.0 * (1.0 - b) * (1.0 - v);                          // overlay
  return mix(lo, hi, step(0.5, b));
}

vec3 screen(vec3 b, vec3 s) { return 1.0 - (1.0 - b) * (1.0 - s); }

// The bevel's highlight screen + shadow multiply on any colour (base or pattern marks).
vec3 light(vec3 c, float hi, float sh) {
  c = screen(c, uHiColor * hi * uHiOp);
  return c * mix(vec3(1.0), uShColor, clamp(sh * uShOp, 0.0, 1.0));
}

int spanIdx(ivec2 q) { return int(taDecodeScalar(texelFetch(TA_RAW2, q, 0), TA_RAW_ENC(2)) + 0.5); }
// Span box of glyph g (uSpan's row of the glyph table), frame px.
void spanBox(int g, out vec2 bmin, out vec2 bmax) {
  vec4 b = texelFetch(TA_RAW3, ivec2(g, uSpan - 1), 0);
  bmin = (b.xy - uFrameMin) * uM + uElemOff - uSpanGrow;
  bmax = (b.zw - uFrameMin) * uM + uElemOff + uSpanGrow;
}
// Frame px from p to the seam where glyph gn takes over from gi as the nearest one (the
// NEAREST_GLYPH_FRAG rule: ink-box distance, ties by centre distance); > 0 on gi's side.
float seamPx(int gi, int gn, vec2 p) {
  vec2 lp = uFrameMin + (p - uElemOff) / uM;
  vec4 bi = texelFetch(TA_RAW3, ivec2(gi, 0), 0), bn = texelFetch(TA_RAW3, ivec2(gn, 0), 0);
  vec2 ci = 0.5 * (bi.xy + bi.zw), cn = 0.5 * (bn.xy + bn.zw);
  vec2 oi = max(abs(lp - ci) - 0.5 * (bi.zw - bi.xy), 0.0), on = max(abs(lp - cn) - 0.5 * (bn.zw - bn.xy), 0.0);
  float di = length(oi), dn = length(on);
  if (max(di, dn) < 1e-4) {
    // Inside both boxes: the perpendicular bisector of the centres.
    vec2 ax = cn - ci;
    float L = length(ax);
    return L > 1e-6 ? dot(0.5 * (ci + cn) - lp, ax) / L * uM : 1e4;
  }
  vec2 gi2 = di > 0.0 ? oi / di * sign(lp - ci) : vec2(0.0);
  vec2 gn2 = dn > 0.0 ? on / dn * sign(lp - cn) : vec2(0.0);
  return (dn - di) / max(length(gn2 - gi2), 1e-3) * uM;
}

void taElementMain() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  float a = texelFetch(TA_TEX0, ip, 0).r;
  if (a <= 0.0) { frag_color = vec4(0.0); return; }
  if (uPunch) { frag_color = vec4(a); return; }
  vec2 p = gl_FragCoord.xy;
  vec2 pa = p + uPxOff;
  float d = uHasSdf ? taDecodeScalar(texelFetch(TA_RAW0, ip, 0), TA_RAW_ENC(0)) : 0.0;

  // Pattern: the frame is axis-aligned, so the motif steps per pixel are exact constants.
  vec4 pat = vec4(0.0);
  vec2 pg = vec2(0.0); // relief: pattern height gradient, height px per frame px
  bool relief = false;
  if (uPatKind > 0) {
    vec2 pm = patternCoord(pa);
    vec2 ex = vec2(uPatXform.x, uPatXform.z), ey = vec2(uPatXform.y, uPatXform.w);
    pat = patternSampleD(pm, ex, ey, uPatKind, uPatSeed);
    if (uPatRelief > 0.0) {
      relief = true;
      // Raised marks stand 6 % of a motif tall at relief 1, whatever the pattern scale.
      float hpx = uPatRelief * 0.06 / max(length(ex), 1e-6);
      pg = vec2(patternSampleD(pm + ex, ex, ey, uPatKind, uPatSeed).z - patternSampleD(pm - ex, ex, ey, uPatKind, uPatSeed).z,
                patternSampleD(pm + ey, ex, ey, uPatKind, uPatSeed).z - patternSampleD(pm - ey, ex, ey, uPatKind, uPatSeed).z) * 0.5 * hpx;
    }
  }

  // Element box: frame px.
  vec2 emin = uBmin, emax = uBmax;
  gBoxC = 0.5 * (emin + emax) + uPxOff;
  gBoxH = max(0.5 * (emax - emin), vec2(0.5));

  // Surface texture joins the bevel height field: lighting, gloss contour and finish all bend.
  bool surface = uTexSurface && uTex > 0 && uTexAmt > 0.0;
  bool lit = uBevel || relief || surface;
  vec3 n = vec3(0.0, 0.0, 1.0);
  if (lit) {
    vec2 g = pg;
    if (uBevel) g += vec2(hFetch(ip + ivec2(1, 0)) - hFetch(ip - ivec2(1, 0)), hFetch(ip + ivec2(0, 1)) - hFetch(ip - ivec2(0, 1))) * 0.5 * uBevelK;
    if (surface) g += textureSlope(pa);
    n = normalize(vec3(-g, 1.0));
  }
  // Refraction: the fill is read where the tilted surface bends the view (< 0: mirrors, like chrome).
  bool refr = uRefrPx != 0.0;
  vec2 pf = refr ? p - n.xy * uRefrPx : p;

  vec2 bmin = emin, bmax = emax;
  int gi = 0, gn = 0;
  if (uSpan > 0) {
    ivec2 mx = textureSize(TA_RAW2, 0) - 1;
    ivec2 q = clamp(ivec2(p - uElemOff), ivec2(0), mx);
    gi = spanIdx(q);
    gn = gi;
    // A 4-neighbour owned by another glyph: this pixel may straddle the seam between the two.
    for (int k = 0; k < 4; k++) {
      ivec2 o4 = k == 0 ? ivec2(1, 0) : k == 1 ? ivec2(-1, 0) : k == 2 ? ivec2(0, 1) : ivec2(0, -1);
      int g = spanIdx(clamp(q + o4, ivec2(0), mx));
      if (g != gi) gn = g;
    }
    spanBox(gi, bmin, bmax);
  }
  float burst = 0.0;
  if (uFillKind == 6 && uHasSdf) {
    float depth = uBurstPx > 0.0 ? uBurstPx : taDecodeScalar(texelFetch(TA_RAW4, ivec2(0), 0), TA_BURST_ENC);
    burst = clamp(-d / max(depth, 1e-3), 0.0, 1.0);
  }
  vec4 fill;
  if (uPicture) {
    vec4 s = refr ? texture(TA_PICTURE, pf / vec2(textureSize(TA_PICTURE, 0))) : texelFetch(TA_PICTURE, ip, 0);
    if (uPictureLinear) {
      // Premultiplied linear: refracted reads off the shape keep the colour here.
      if (s.a < 1e-4) s = texelFetch(TA_PICTURE, ip, 0);
      fill = vec4(toSrgb(s.rgb / max(s.a, 1e-4)), 1.0);
    } else fill = vec4(s.rgb, 1.0);
  } else fill = fillColor(pf, bmin, bmax, burst);
  if (gn != gi) {
    // Antialias the span seam: blend in the neighbour's fill by its share of this pixel.
    vec2 nmin, nmax;
    spanBox(gn, nmin, nmax);
    if (nmin != bmin || nmax != bmax) fill = mix(fill, fillColor(pf, nmin, nmax, burst), clamp(0.5 - seamPx(gi, gn, p), 0.0, 1.0));
  }
  vec3 c = fill.rgb;

  // Glass body, in linear light: what is seen through takes the glass colour and deepens like
  // thick glass; steep slopes darken (Fresnel rim); light entering on the lit side focuses into a
  // caustic band inside the far edge. Rims and caustics are more opaque than the body.
  float glassA = 1.0;
  if (uGlass) {
    vec3 base = mix(toLinear(c), toLinear(uGlassColor), uGlassTint);
    vec3 gc = base * base * 0.3 + base * 0.14;
    float slope = clamp(1.0 - n.z, 0.0, 1.0);
    float fres = smoothstep(0.0, 0.7, slope);
    gc *= 1.0 - fres * uGlassEdge * 0.85;
    float caus = 0.0;
    if (uBevel) {
      vec2 l2 = normalize(uLight.xy + 1e-6);
      float hHere = hFetch(ip);
      float hBack = hFetch(ivec2(p - l2 * uGlassCw));
      caus = clamp(1.0 - hBack, 0.0, 1.0) * smoothstep(0.05, 0.6, hHere) * (1.0 - fres * 0.6);
      gc += mix(base, vec3(1.0), 0.3) * caus * caus * uGlassCaustic * 1.8;
      gc += base * 0.15 * smoothstep(0.5, 1.0, hHere) * uGlassCaustic;
    }
    // The sky on upward-facing slopes.
    gc += vec3(0.55) * smoothstep(0.1, 0.8, n.y) * slope;
    glassA = clamp(uGlassOp + (1.0 - uGlassOp) * (fres * uGlassEdge * 0.8 + caus * uGlassCaustic * 0.6), 0.0, 1.0);
    c = toSrgb(gc);
  }

  // Mirror the view ray (straight into the screen) off the surface into the panorama; screen
  // space, so the sky stays up when the layer turns. The fill keeps its hue under colorize.
  if (uRefl) {
    vec3 ns = vec3(uReflM * n.xy, n.z);
    vec3 R = vec3(2.0 * ns.z * ns.xy, 2.0 * ns.z * ns.z - 1.0);
    vec2 uv = vec2(atan(R.x, R.z) / 6.2831853 + 0.5 + uReflRot, 0.5 - asin(clamp(R.y, -1.0, 1.0)) / 3.1415927);
    vec3 rc = texture(TA_REFLECTION, uv).rgb;
    float l = dot(rc, vec3(0.299, 0.587, 0.114));
    // Coloured glass: dark reflections deepen the fill, mid tones keep its full hue and only the
    // brightest ones run to white (a squared ramp), so saturated colours never wash to pastel.
    float w = max(2.0 * l - 1.0, 0.0);
    vec3 tinted = l < 0.5 ? 2.0 * c * l : mix(c, vec3(1.0), w * w);
    c = mix(c, mix(rc, tinted, uReflTint), uReflAmt);
  }

  float hi = 0.0, sh = 0.0;
  if (lit) {
    float s = dot(n, uLight) - uLight.z;
    hi = contour(s > 0.0 ? s / max(1.0 - uLight.z, 0.05) : 0.0);
    sh = contour(s < 0.0 ? -s / max(uLight.z, 0.05) : 0.0);
    c = light(c, hi, sh);
  }

  if (uFinish) {
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(c, rampAt(1.0, lum), uFinishAmt);
  }

  // Marks, then color2 over them; uPatShade keeps that much of the bevel lighting on both.
  if (uPatKind > 0) {
    vec3 m1 = uPatColor, m2 = uPatColor2;
    if (lit) {
      m1 = mix(m1, light(m1, hi, sh), uPatShade);
      m2 = mix(m2, light(m2, hi, sh), uPatShade);
    }
    c = mix(c, m1, pat.x * uPatOpacity);
    c = mix(c, m2, pat.y * uPatOpacity);
  }

  // Satin: |difference| of two blurred copies of the silhouette shifted ±distance, through the
  // contour (inverted like Photoshop's default), blended in its colour.
  if (uSatin) {
    vec2 ts = vec2(textureSize(TA_TEX1, 0));
    float s1 = texture(TA_TEX1, (p + uSatinOff) / ts).r;
    float s2 = texture(TA_TEX1, (p - uSatinOff) / ts).r;
    float v = taLut16(TA_LUTS, 3, abs(s1 - s2));
    if (uSatinInvert) v = 1.0 - v;
    vec3 sc = uSatinColor;
    vec3 bl = uSatinBlend == 1 ? c * sc
            : uSatinBlend == 2 ? screen(c, sc)
            : uSatinBlend == 3 ? mix(2.0 * c * sc, 1.0 - 2.0 * (1.0 - c) * (1.0 - sc), step(0.5, c))
            : sc;
    c = mix(c, bl, clamp(v * uSatinOp, 0.0, 1.0));
  }

  if (uTex > 0 && uTexAmt > 0.0 && !uTexSurface) c = mix(c, clamp(blendTex(c, textureValue(pa)), 0.0, 1.0), uTexAmt);

  if (uSheen) c += uSheenColor * sheenShape(pa) * uSheenAmt * (0.4 + 0.6 * c);

  if (uGlow && uHasSdf) {
    float t = clamp(max(-d, 0.0) / max(uGlowPx, 0.5), 0.0, 1.0);
    float gi = (1.0 - t) * (1.0 - t);
    float n = hash12(pa + 13.7);
    gi *= mix(1.0, n * 2.0, uGlowNoise);
    c = screen(c, uGlowColor * clamp(gi * uGlowOp, 0.0, 1.0));
  }

  if (uCap) {
    // Band coordinates along uCapDir (0 = the band's top edge of the element box, 1 = bottom)
    // and across it (-1..1), so the cap tilts with Gloss2D.angle.
    vec2 cc = gBoxC;
    vec2 hs = gBoxH;
    vec2 dn = uCapDir, ac = vec2(-dn.y, dn.x);
    float ed = abs(dn.x) * hs.x + abs(dn.y) * hs.y;
    float ea = abs(ac.x) * hs.x + abs(ac.y) * hs.y;
    float yf = (dot(pa - cc, dn) + ed) / (2.0 * ed);
    float u = dot(pa - cc, ac) / ea;
    float edge = uCapPos + uCapCurve * 0.25 * (1.0 - u * u);
    float sw = max(uCapSoft, 0.002) * 0.5;
    float cap = 1.0 - smoothstep(edge - sw, edge + sw, yf);
    cap *= mix(1.0, 0.55, clamp(yf / max(edge, 1e-3), 0.0, 1.0));
    c = mix(c, vec3(1.0), clamp(cap * uCapOp, 0.0, 1.0));
  }

  float alpha = a * fill.a * uOpacity;
  // Glass: the body's own opacity; the bevel highlight stays visible on clear glass.
  if (uGlass) alpha *= clamp(glassA + hi * uHiOp, 0.0, 1.0);
  frag_color = vec4(toLinear(c) * alpha, alpha);
}
