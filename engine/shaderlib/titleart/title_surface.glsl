// Normal detail of 3D titles (docs/specs/S86-title-3d.md R22), projected front on in layer
// space: crumple (Voronoi facets with random slopes, 5x5 search, 3x3 in TL_SURFACE_LITE),
// hammered (radial dents), liquid (domain-warped gradient noise after Inigo Quilez, MIT, as
// Pachislop credits it) and flakes (one jittered disc per 3D hash cell, tilted at most 0.55,
// antialiased with fwidth). Ported from Pachislop src/engine/three/materials.ts by SafEight,
// used with the author's permission. The hashes are PCG (Jarzynski and Olano, JCGT 2020).
// Reads `tl`.

uvec3 tlPcg3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

highp vec3 tlHash3(highp vec3 p, float seed) {
    uvec3 u = uvec3(ivec3(floor(p)) + ivec3(int(seed) * 7919, int(seed) * 104729, 0));
    return vec3(tlPcg3(u) >> 8u) * (1.0 / 16777216.0);
}

vec3 tlBendNormal(vec3 N, vec3 tilt) {
    return normalize(N + tl.axisX.xyz * tilt.x + tl.axisY.xyz * tilt.y);
}

#ifdef TL_SURFACE_KIND
float tlNoise(highp vec2 p, float seed) {
    highp vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dot(tlHash3(vec3(i, 0.0), seed).xy * 2.0 - 1.0, f);
    float b = dot(tlHash3(vec3(i + vec2(1.0, 0.0), 0.0), seed).xy * 2.0 - 1.0, f - vec2(1.0, 0.0));
    float c = dot(tlHash3(vec3(i + vec2(0.0, 1.0), 0.0), seed).xy * 2.0 - 1.0, f - vec2(0.0, 1.0));
    float d = dot(tlHash3(vec3(i + vec2(1.0, 1.0), 0.0), seed).xy * 2.0 - 1.0, f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// the slope of one octave of the surface at layer uv (in feature cells)
vec2 tlSurfaceOctave(highp vec2 uv, float seed) {
#if TL_SURFACE_KIND == 1
    // crumple: the nearest jittered site's random slope, blended over the borders
    #ifdef TL_SURFACE_LITE
        const int R = 1;
    #else
        const int R = 2;
    #endif
    highp vec2 cell = floor(uv);
    float best = 1e9, second = 1e9;
    vec2 slope = vec2(0.0), slope2 = vec2(0.0);
    for (int y = -R; y <= R; y++) {
        for (int x = -R; x <= R; x++) {
            highp vec2 c = cell + vec2(float(x), float(y));
            vec3 h = tlHash3(vec3(c, 1.0), seed);
            float d = length(c + h.xy - uv);
            vec2 s = (tlHash3(vec3(c, 2.0), seed).xy * 2.0 - 1.0);
            if (d < best) {
                second = best;
                slope2 = slope;
                best = d;
                slope = s;
            } else if (d < second) {
                second = d;
                slope2 = s;
            }
        }
    }
    float w = fwidth(uv.x) + fwidth(uv.y);
    float k = smoothstep(0.0, max(w, 1e-4), second - best);
    return mix(mix(slope, slope2, 0.5), slope, k);
#elif TL_SURFACE_KIND == 2
    // hammered: a round dent per jittered cell
    highp vec2 cell = floor(uv);
    vec3 h = tlHash3(vec3(cell, 3.0), seed);
    vec2 d = uv - (cell + 0.25 + h.xy * 0.5);
    float r = length(d);
    return r < 0.5 ? d / 0.5 * (1.0 - r / 0.5) * 2.0 : vec2(0.0);
#else
    // liquid: domain-warped gradient noise, its finite-difference gradient
    highp vec2 q = uv + vec2(tlNoise(uv, seed), tlNoise(uv + vec2(5.2, 1.3), seed)) * 0.8;
    const float e = 0.05;
    float n0 = tlNoise(q, seed);
    return vec2(tlNoise(q + vec2(e, 0.0), seed) - n0, tlNoise(q + vec2(0.0, e), seed) - n0) / e * 0.35;
#endif
}

vec3 tlSurfaceTilt(highp vec2 uv, float seed) {
    vec2 s = tlSurfaceOctave(uv, seed);
#ifndef TL_SURFACE_LITE
    s = s * 0.7 + tlSurfaceOctave(uv * 2.37 + vec2(17.0, 3.0), seed + 1.0) * 0.3;
#endif
    return vec3(s, 0.0);
}
#endif

#ifdef TL_FLAKES
vec3 tlFlakeTilt(highp vec3 p, float cell) {
    highp vec3 q = p / max(cell, 1e-4);
    highp vec3 c = floor(q);
    vec3 h = tlHash3(c, 11.0);
    vec3 centre = c + 0.2 + h * 0.6;
    float d = length(q.xy - centre.xy);
    float radius = 0.35;
    float aa = max(fwidth(q.x) + fwidth(q.y), 1e-4);
    float inside = 1.0 - smoothstep(radius - aa, radius + aa, d);
    vec2 tilt = (tlHash3(c, 13.0).xy * 2.0 - 1.0) * 0.55;
    return vec3(tilt * inside, 0.0);
}
#endif
