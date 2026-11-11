// Fractal Noise (docs/specs/S83-title-effects-generators.md §6.7.16, new): octaves of Pachislop's
// value noise (tfx_noise.glsl), turbulent and ridged sums, or Worley cells (F2 - F1), coloured
// through the gradient ramp in TA_TEX0. `evolution` moves the noise without a clock: each octave
// blends two slices of the plane keyed by whole evolution steps, and cell points orbit their
// cells, so a keyed evolution loops smoothly.
//
// p6 = (offset x, y, scale, aspect); p7 = (rotation cos, sin, complexity, noise kind 0-3);
// p8 = (contrast, brightness, evolution, seed).

#ifndef TITLEART_GEN_FRACTAL_NOISE_GLSL
#define TITLEART_GEN_FRACTAL_NOISE_GLSL

const int FN_MAX_OCTAVES = 8;

// value noise at p, `evo` slices deep: two slices of the plane blended (seamless in evo)
float fn_evolving(vec2 p, float evo, float octave) {
    float e0 = floor(evo);
    float t = evo - e0;
    t = t * t * (3.0 - 2.0 * t);
    vec2 a = hash22(vec2(e0, octave + 0.5)) * 517.0;
    vec2 b = hash22(vec2(e0 + 1.0, octave + 0.5)) * 517.0;
    return mix(vnoise(p + a), vnoise(p + b), t);
}

// Worley F2 - F1 (cell edges), points orbiting inside their cells with evolution
float fn_cells(vec2 p, float evo) {
    vec2 i = floor(p), f = p - i;
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 c = vec2(float(x), float(y));
            vec2 h = hash22(i + c);
            vec2 o = 0.5 + 0.4 * vec2(sin(6.2831853 * (h.x + evo)), cos(6.2831853 * (h.y + evo)));
            float d = length(c + o - f);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    return clamp(f2 - f1, 0.0, 1.0);
}

vec4 gen_fractalNoise() {
    vec2 offset = ta.p[6].xy;
    float scale = max(ta.p[6].z, 1e-3);
    float aspect = max(ta.p[6].w, 1e-3);
    vec2 rot = ta.p[7].xy;
    int octaves = int(ta.p[7].z);
    int kind = int(ta.p[7].w);
    float contrast = ta.p[8].x, brightness = ta.p[8].y, evo = ta.p[8].z, seed = ta.p[8].w;

    // the layer-space point in noise units: scrolled, turned clockwise by `rotation`, stretched
    vec2 s = gen_spacePos() - offset;
    vec2 q = vec2(rot.x * s.x - rot.y * s.y, rot.y * s.x + rot.x * s.y);
    q = vec2(q.x / aspect, q.y) / scale + hash22(vec2(seed, 7.0)) * 1000.0;

    float v = 0.0;
    if (kind == 3) {
        float amp = 0.5, total = 0.0;
        for (int o = 0; o < FN_MAX_OCTAVES; o++) {
            if (o >= octaves) break;
            v += amp * fn_cells(q, evo * 0.05 + float(o) * 0.37);
            total += amp;
            q = q * 2.03 + 17.1;
            amp *= 0.5;
        }
        v /= total;
    } else {
        float amp = 0.5, total = 0.0, weight = 1.0;
        for (int o = 0; o < FN_MAX_OCTAVES; o++) {
            if (o >= octaves) break;
            float n = fn_evolving(q, evo, float(o));
            if (kind == 1) {
                n = abs(2.0 * n - 1.0);
            } else if (kind == 2) {
                n = 1.0 - abs(2.0 * n - 1.0);
                n *= n;
                // ridges sharpen where the coarser octaves already ridged
                n *= weight;
                weight = clamp(n * 2.0, 0.0, 1.0);
            }
            v += amp * n;
            total += amp;
            q = q * 2.03 + 17.1;
            amp *= 0.5;
        }
        v /= total;
    }
    v = clamp((v - 0.5) * contrast + 0.5 + brightness, 0.0, 1.0);
    vec4 c = gen_ramp(TA_TEX0, v);
    return vec4(c.rgb * c.a, c.a);
}

#endif // TITLEART_GEN_FRACTAL_NOISE_GLSL
