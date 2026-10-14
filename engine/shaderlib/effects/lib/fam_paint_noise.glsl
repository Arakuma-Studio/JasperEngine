// HTML UI effects, paint family (p5-fx-paint): noise fields the paint effects share. Ported from
// the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt):
// noise.ts perlin13 and curl22z, fields.ts domainWarp2/chainedWarpStep/spiralScatter/scatterField
// and the golden-angle fbmGated, cellular.ts cellHash2 and the drifting nearest-2 fold. The
// constants are the kit's, bit for bit.
#ifndef FAM_PAINT_NOISE_GLSL
#define FAM_PAINT_NOISE_GLSL

#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"

#define FAM_GOLDEN 2.39996322972865332
#define FAM_GOLDEN_CONJ 0.6180339887

// noise.ts perlinGrad3: dot(f - o, normalize(hash33(i + o) - 0.5))
float fam_perlin_grad3(vec3 i, vec3 f, vec3 o){
    return dot(f - o, normalize(jfx_hash33(i + o) - 0.5));
}

// noise.ts perlin13: 3D gradient noise, roughly [0, 1] (x 0.7 + 0.5)
float fam_perlin13(vec3 p){
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a0 = fam_perlin_grad3(i, f, vec3(0.0, 0.0, 0.0));
    float b0 = fam_perlin_grad3(i, f, vec3(1.0, 0.0, 0.0));
    float c0 = fam_perlin_grad3(i, f, vec3(0.0, 1.0, 0.0));
    float d0 = fam_perlin_grad3(i, f, vec3(1.0, 1.0, 0.0));
    float a1 = fam_perlin_grad3(i, f, vec3(0.0, 0.0, 1.0));
    float b1 = fam_perlin_grad3(i, f, vec3(1.0, 0.0, 1.0));
    float c1 = fam_perlin_grad3(i, f, vec3(0.0, 1.0, 1.0));
    float d1 = fam_perlin_grad3(i, f, vec3(1.0, 1.0, 1.0));
    float z0 = mix(mix(a0, b0, u.x), mix(c0, d0, u.x), u.y);
    float z1 = mix(mix(a1, b1, u.x), mix(c1, d1, u.x), u.y);
    return mix(z0, z1, u.z) * 0.7 + 0.5;
}

// noise.ts curl22z: the central-difference gradient of perlin13 on the z slice
vec2 fam_curl22z(vec2 p, float z){
    vec2 a = vec2(fam_perlin13(vec3(p + vec2(0.1, 0.0), z)), fam_perlin13(vec3(p + vec2(0.0, 0.1), z)));
    vec2 b = vec2(fam_perlin13(vec3(p - vec2(0.1, 0.0), z)), fam_perlin13(vec3(p - vec2(0.0, 0.1), z)));
    return (a - b) / 0.1 * 0.5;
}

// fields.ts domainWarp2 over mxNoiseFloat3 with the Inigo Quilez offsets: two levels, both
// displacing the original position; returns the position to sample the field at
vec3 fam_warp2(vec3 pos, float t, float amount){
    float qx = jfx_mx_noise3(pos + vec3(t * 0.3, t * -0.2, 0.0));
    float qy = jfx_mx_noise3(pos + vec3(5.2, 1.3, t * 0.25));
    vec3 warped1 = pos + vec3(qx, qy, 0.0) * amount;
    float rx = jfx_mx_noise3(warped1 + vec3(1.7, 9.2, t * 0.15));
    float ry = jfx_mx_noise3(warped1 + vec3(8.3, 2.8, t * 0.13));
    return pos + vec3(rx, ry, 0.0) * amount;
}

// fields.ts chainedWarpStep: one warp level over a chained position: (warped xy, nx, ny)
vec4 fam_chained_warp_step(vec2 pos, float scale, float z, vec2 offA, vec2 offB, float k, float strength){
    vec3 wp = vec3(pos.x * scale, pos.y * scale, z);
    float nx = jfx_mx_noise3(wp + vec3(offA, 0.0));
    float ny = jfx_mx_noise3(wp + vec3(offB, 0.0));
    return vec4(pos.x + nx * k * strength, pos.y + ny * k * strength, nx, ny);
}

// fields.ts spiralScatter: point fi of a drifting golden-spiral constellation, in
// aspect-corrected uv ([0, aspect] x [0, 1])
vec2 fam_spiral_scatter(float fi, float count, float seed, float drift, float aspect, float t){
    float theta = fi * FAM_GOLDEN + jfx_hash11(seed * 0.7311 + 1.618) * JFX_TAU;
    float r = sqrt((fi + 0.5) / count);
    vec2 j = (jfx_hash22(vec2(fi * 3.77 + 0.917, seed * 1.093 + 2.236)) - 0.5) * 0.26;
    vec2 ph = jfx_hash22(vec2(fi * 7.13 + 1.618, seed * 0.531 + 3.71)) * JFX_TAU;
    float w1 = 0.5 + jfx_hash11(fi * 5.417 + seed * 0.291 + 0.917) * 0.7;
    float dx = sin(t * 0.11 * w1 + ph.x);
    float dy = cos(t * 0.11 * w1 * 1.37 + ph.y);
    float nx = cos(theta) * r * 0.62 + j.x + dx * drift * 0.16;
    float ny = sin(theta) * r * 0.62 + j.y + dy * drift * 0.16;
    return vec2((0.5 + nx) * aspect, 0.5 + ny);
}

// fields.ts scatterField: the constellation's golden-ratio values blended by inverse distance to
// the power `power` (count gates the fixed 8)
float fam_scatter_field(vec2 q, float count, float seed, float drift, float aspect, float t, float power){
    float num = 0.0;
    float den = 0.0;
    float seedShuffle = jfx_hash11(seed * 0.531 + 0.77);
    for (int i = 0; i < 8; i++){
        float fi = float(i);
        float gate = fi < count - 0.5 ? 1.0 : 0.0;
        vec2 anchor = fam_spiral_scatter(fi, count, seed, drift, aspect, t);
        float dist = max(length(q - anchor), 0.01);
        float w = gate / pow(dist, power);
        float ti = fract(fi * FAM_GOLDEN_CONJ + seedShuffle);
        num = num + w * ti;
        den = den + w;
    }
    return num / max(den, 0.000001);
}

// cellular.ts cellHash2: the sin-fract cell hash
vec2 fam_cell_hash2(vec2 p){
    vec2 h = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(h) * 43758.5453);
}

// cellular.ts nearest2Cells (length, no jitter control): the two nearest drifting cell points,
// (d1, d2)
vec2 fam_voronoi_nearest2(vec2 scaledUV, float animT, float seed){
    vec2 cell = floor(scaledUV);
    vec2 localUV = fract(scaledUV);
    float d1 = 10.0;
    float d2 = 10.0;
    for (int ny = -1; ny <= 1; ny++){
        for (int nx = -1; nx <= 1; nx++){
            vec2 offset = vec2(float(nx), float(ny));
            vec2 h = fam_cell_hash2(vec2(cell.x + offset.x + seed, cell.y + offset.y + seed));
            float pointX = clamp(h.x + sin(animT + h.x * JFX_TAU) * 0.15, 0.05, 0.95);
            float pointY = clamp(h.y + cos(animT * 0.7 + h.y * JFX_TAU) * 0.15, 0.05, 0.95);
            float dd = length(vec2(localUV.x - (offset.x + pointX), localUV.y - (offset.y + pointY)));
            d2 = min(d2, max(dd, d1));
            d1 = min(d1, dd);
        }
    }
    return vec2(d1, d2);
}

// fields.ts fbmGated (golden-angle drift) over mxNoiseFloat2: (accumulated, total weight), each
// octave drifting in its own direction; octaveCount gates the fixed 8
vec2 fam_fbm_golden_mx2(vec2 base, float baseFreq, float lacunarity, float persistence, float t, vec2 seedOffset, float octaveCount){
    int n = int(octaveCount);
    float freq = baseFreq;
    float weight = 1.0;
    float accumulated = 0.0;
    float totalWeight = 0.0;
    for (int i = 0; i < 8; i++){
        float fi = float(i);
        vec2 coord = vec2(base.x * freq + t * cos(fi * FAM_GOLDEN) + seedOffset.x,
                          base.y * freq + t * sin(fi * FAM_GOLDEN) + seedOffset.y);
        float gate = i < n ? 1.0 : 0.0;
        // the gated octaves contribute exactly 0 (their noise is not read)
        if (gate > 0.0)
            accumulated = accumulated + jfx_mx_noise2(coord) * weight;
        totalWeight = totalWeight + weight * gate;
        freq = freq * lacunarity;
        weight = weight * persistence;
    }
    return vec2(accumulated, totalWeight);
}

#endif // FAM_PAINT_NOISE_GLSL
