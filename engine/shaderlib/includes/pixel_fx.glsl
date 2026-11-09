// Pixel FX helpers (docs/specs/S48-pixel-fx.md D1, D4), shared by library:sprite and the screen
// transition. Every function is integer or plain float maths on texel coordinates (no texture, no
// derivative), so all backends agree, and engine/core/pixelart/PixelFxKey.cpp mirrors it on the
// CPU (S49 spawns dissolve embers where the GPU burns). The ordered-dither table is S43's
// includes/bayer.glsl, the family's only one.

#ifndef PIXEL_FX_GLSL
#define PIXEL_FX_GLSL

// PCG2D (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020): 32-bit, highp uint,
// which GLSL ES 3.00 guarantees in fragment shaders
uvec2 pfxPcg2d(uvec2 v){
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v = v ^ (v >> 16u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v = v ^ (v >> 16u);
    return v;
}

// a 32-bit hash of a texel (non-negative coordinates) and a seed
uint pfxHash(uvec2 texel, uint seed){
    return pfxPcg2d(texel ^ uvec2(seed, seed * 2654435769u)).x;
}

// the hash as [0, 1): the top 24 bits, exact in a float on every backend
float pfxHash01(uvec2 texel, uint seed){
    return float(pfxHash(texel, seed) >> 8u) * (1.0 / 16777216.0);
}

// Value noise in [0, 1): the hashes at whole cells of `cell` texels, bilinear between them. p is
// a texel-space position (non-negative), usually a texel centre.
float pfxValueNoise(vec2 p, float cell, uint seed){
    vec2 q = p / max(cell, 1.0);
    vec2 i = floor(q);
    vec2 f = q - i;
    uvec2 c = uvec2(i);
    float a = pfxHash01(c, seed);
    float b = pfxHash01(c + uvec2(1u, 0u), seed);
    float d = pfxHash01(c + uvec2(0u, 1u), seed);
    float e = pfxHash01(c + uvec2(1u, 1u), seed);
    float lo = a + (b - a) * f.x;
    float hi = d + (e - d) * f.x;
    return lo + (hi - lo) * f.y;
}

// pfxValueNoise and its exact gradient (per texel) inside the cell: the dissolve's band widths
// come from it without a derivative instruction
float pfxValueNoiseGrad(vec2 p, float cell, uint seed, out vec2 grad){
    float size = max(cell, 1.0);
    vec2 q = p / size;
    vec2 i = floor(q);
    vec2 f = q - i;
    uvec2 c = uvec2(i);
    float a = pfxHash01(c, seed);
    float b = pfxHash01(c + uvec2(1u, 0u), seed);
    float d = pfxHash01(c + uvec2(0u, 1u), seed);
    float e = pfxHash01(c + uvec2(1u, 1u), seed);
    float lo = a + (b - a) * f.x;
    float hi = d + (e - d) * f.x;
    grad = vec2((b - a) + ((e - d) - (b - a)) * f.y, hi - lo) / size;
    return lo + (hi - lo) * f.y;
}

// The dissolve (S48 R3, D4). Every texel of the frame has a key in [0, 1): it burns once the front
// passes it. With a direction (texels, y up) the key is the texel's height across the frame along
// it, displaced by value noise of `cell` texels (by up to 0.75 x noise x cell texels, so up to a
// noise of about 0.6 the key keeps rising along the direction) plus a jitter shared by the texels
// of each line along the direction (a ragged front of straight columns). Without one ((0, 0)) the
// key is the noise alone plus a jitter per texel. Positions are texels of the frame from its
// bottom-left corner (texel centres at + 0.5).

// the smooth part of the key at a position: the value in [0, range), its gradient per texel and
// the frame's extent along the direction (the cell without one)
float pfxDissolveSmooth(vec2 pos, vec2 size, vec2 dir, float noise, float cell, uint seed,
                        out vec2 gradient, out float range, out float extent){
    vec2 noiseGrad;
    // offset: the noise's cells stay at non-negative coordinates around the frame
    float n = pfxValueNoiseGrad(pos + 1024.0, cell, seed, noiseGrad);
    float len = length(dir);
    if (len > 1e-6){
        vec2 d = dir / len;
        float lo = min(0.0, d.x * size.x) + min(0.0, d.y * size.y);
        float hi = max(0.0, d.x * size.x) + max(0.0, d.y * size.y);
        extent = max(hi - lo, 1.0);
        float amount = 1.5 * clamp(noise, 0.0, 1.0) * max(cell, 1.0) / extent;
        range = 1.0 + amount;
        gradient = d / extent + noiseGrad * amount;
        return (dot(pos, d) - lo) / extent + amount * n;
    }
    extent = max(cell, 1.0);
    range = 1.0;
    gradient = noiseGrad;
    return n;
}

// the jitter's hash of a texel: one per line along the direction, else one per texel (its own
// seed, so it does not line up with the noise)
float pfxDissolveJitter(ivec2 texel, vec2 dir, uint seed){
    uint jitterSeed = seed ^ 1759714724u;
    float len = length(dir);
    if (len > 1e-6){
        vec2 d = dir / len;
        float line = floor(dot(vec2(texel) + 0.5, vec2(-d.y, d.x)) + 1024.0);
        return pfxHash01(uvec2(uint(line), 0u), jitterSeed);
    }
    return pfxHash01(uvec2(texel + ivec2(1024)), jitterSeed);
}

// the key of a texel of the frame, in [0, 1)
float pfxDissolveKey(ivec2 texel, ivec2 frameSize, vec2 dir, float noise, float cell, float jitter, uint seed){
    vec2 gradient;
    float range;
    float extent;
    float smoothKey = pfxDissolveSmooth(vec2(texel) + 0.5, vec2(max(frameSize, ivec2(1))), dir, noise, cell, seed, gradient, range, extent);
    float j = max(jitter, 0.0);
    return (smoothKey + j * pfxDissolveJitter(texel, dir, seed)) / (range + j);
}

// the front a progress puts the keys against: progress 0 below every key and the bands (span:
// their width in key units), 1 above every key
float pfxDissolveFront(float progress, float span){
    return progress * (1.0 + span) - span;
}

// Where a texel of the frame stands against the front at a progress (0 < progress < 1): negative
// once burnt; in [0, edge) in the edge band, its floor the band's step from the front; in
// [edge, edge + scorch) scorched; above that untouched. With a direction the bands are exact whole
// texels along it: a texel is in the edge band when the texel `edge` steps back toward the front
// is burnt (the field's own values there, no derivative); without one they come from the field
// rule (pixelfx's _lib.js field: the key difference over the gradient, held within 0.45 to 3 times
// a straight front's).
float pfxDissolveDistance(ivec2 texel, ivec2 frameSize, vec2 dir, float noise, float cell, float jitter, uint seed,
                          float progress, float edge, float scorch){
    vec2 size = vec2(max(frameSize, ivec2(1)));
    vec2 centre = vec2(texel) + 0.5;
    vec2 gradient;
    float range;
    float extent;
    float smoothKey = pfxDissolveSmooth(centre, size, dir, noise, cell, seed, gradient, range, extent);
    float j = max(jitter, 0.0);
    float scale = 1.0 / (range + j);
    float shift = j * pfxDissolveJitter(texel, dir, seed);
    float key = (smoothKey + shift) * scale;
    float nominal = scale / extent;
    float front = pfxDissolveFront(progress, (edge + scorch) * nominal);
    if (key < front)
        return -1.0;
    float len = length(dir);
    if (len <= 1e-6)
        return (key - front) / clamp(length(gradient) * scale, 0.45 * nominal, 3.0 * nominal);
    vec2 d = dir / len;
    vec2 ignored;
    float ignoredRange;
    float ignoredExtent;
    if (edge > 0.0){
        float back = (pfxDissolveSmooth(centre - d * edge, size, dir, noise, cell, seed, ignored, ignoredRange, ignoredExtent) + shift) * scale;
        if (back < front)
            return clamp((key - front) / max(key - back, 1e-6) * edge, 0.0, edge - 0.001);
    }
    if (scorch > 0.0){
        float back = (pfxDissolveSmooth(centre - d * (edge + scorch), size, dir, noise, cell, seed, ignored, ignoredRange, ignoredExtent) + shift) * scale;
        if (back < front)
            return edge;
    }
    return edge + scorch + 1.0;
}

#endif
