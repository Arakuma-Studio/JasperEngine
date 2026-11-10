// Magic Circle (魔法陣 / HUD rings), ported from Pachislop src/generators/rings/magicCircle.ts by
// SafEight, used with the author's permission: up to six concentric rows (ring, dashes, ticks,
// text, polygon, stars, chevrons, random arcs, a hex-grid band) as anti-aliased distance fields
// in the circle's plane, spun, optionally laid back on a floor plane, coloured centre -> edge.
//
// p6 = (centre x, y, spin cos, sin); p7 = (tilt sin, cos, focal length, row count); p8 = (radius,
// fill 0-1, fill radius, seed); p9 = (atlas columns, rows, glyph count (0 = no atlas), 0);
// p10 + i = row i: (type + 16 x count, radius px, size px, angle rad). All in layer px, angles in
// Pachislop's convention (counter-clockwise). TA_TEX0 = the gradient ramp, TA_TEX1 = the glyph
// atlas (R8 coverage, rows top first: generators/RingGenerators.cpp, analysis/GlyphAtlas.h).

#ifndef TITLEART_GEN_MAGIC_CIRCLE_GLSL
#define TITLEART_GEN_MAGIC_CIRCLE_GLSL

const int MC_MAX_ROWS = 6;
const float MC_TAU = 6.28318531;

vec2 mc_Jx, mc_Jy;  // circle-plane px per target px along x / y

// plane px per target px along unit plane direction g: the anti-aliasing width of a distance
// whose gradient is g
float mc_aaAlong(vec2 g) {
    return max(length(vec2(dot(mc_Jx, g), dot(mc_Jy, g))), 1e-4);
}

float mc_cover(float d, float aa) {
    return clamp(0.5 - d / aa, 0.0, 1.0);
}

vec2 mc_closest(vec2 p, vec2 a, vec2 b) {
    vec2 ba = b - a;
    return a + ba * clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
}

// pointy-top hex lattice of unit flat-to-flat width: (offset from the cell centre, cell id)
vec4 mc_hexCell(vec2 p) {
    const vec2 s = vec2(1.0, 1.7320508);
    vec4 hc = floor(vec4(p, p - vec2(0.5, 1.0)) / s.xyxy) + 0.5;
    vec4 h = vec4(p - hc.xy * s, p - (hc.zw + 0.5) * s);
    return dot(h.xy, h.xy) < dot(h.zw, h.zw) ? vec4(h.xy, hc.xy) : vec4(h.zw, hc.zw + 0.5);
}

// coverage of row i at plane point pl (polar radius r, radial unit vector er; aaR / aaT the
// anti-aliasing widths across / along the rings)
float mc_rowCover(int i, vec2 pl, float r, vec2 er, float aaR, float aaT) {
    vec4 A = ta.p[10 + i];
    float packed = floor(A.x + 0.5);
    float n = max(floor(packed / 16.0), 1.0);
    int type = int(packed - 16.0 * floor(packed / 16.0) + 0.5);
    float R = A.y, s = A.z;
    float a0 = 0.5 * TA_PI + A.w;           // slots start at the top...
    float th = a0 - atan(pl.y, pl.x);       // ...and run clockwise, the reading direction of text rows
    float phi = th / MC_TAU * n;            // slot coordinate: slot k is centred on phi = k
    float slot = MC_TAU / n;
    if (type == 0) return mc_cover(abs(r - R) - 0.5 * s, aaR);
    if (type == 1) {
        float pc = phi - floor(phi + 0.5);
        return mc_cover(abs(r - R) - 0.5 * s, aaR) * mc_cover((abs(pc) - 0.3) * slot * r, aaT);
    }
    if (type == 2) {
        // ticks hang inward from R; long majors every 5th / 4th / 3rd tick, whichever divides the
        // count (so they stay regular all round), once there are enough ticks to count
        float k = floor(phi + 0.5);
        float every = mod(n, 5.0) < 0.5 ? 5.0 : mod(n, 4.0) < 0.5 ? 4.0 : mod(n, 3.0) < 0.5 ? 3.0 : 0.0;
        float L = n >= 20.0 && every > 0.0 && mod(mod(k, n), every) < 0.5 ? 1.7 * s : s;
        float w = max(0.12 * s, 1.0);
        return mc_cover(abs(r - (R - 0.5 * L)) - 0.5 * L, aaR) * mc_cover(abs(phi - k) * slot * r - 0.5 * w, aaT);
    }
    if (type == 3 || type == 7) {
        // upright frame of slot k: x along the clockwise tangent, y outward from the row radius
        float k = floor(phi + 0.5);
        float a = a0 - k * slot;
        vec2 ur = vec2(cos(a), sin(a)), ux = vec2(ur.y, -ur.x);
        vec2 lc = vec2(dot(pl, ux), dot(pl, ur) - R);
        if (type == 3) {
            vec3 glyphs = ta.p[9].xyz;
            if (glyphs.z < 0.5) return 0.0;
            vec2 uv = lc / s + 0.5;
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
            // slot 0 (the top) takes the first glyph; the string repeats clockwise. The atlas'
            // rows run top first, so a glyph's top (uv.y = 1) is its cell's first row
            float g = mod(floor(mod(k, n) + 0.5), glyphs.z);
            float gx = mod(g, glyphs.x), gy = floor(g / glyphs.x);
            return textureLod(TA_TEX1, vec2((gx + uv.x) / glyphs.x, (gy + 1.0 - uv.y) / glyphs.y), 0.0).r;
        }
        // chevron pointing at the centre
        vec2 pa = vec2(-0.5 * s, 0.25 * s), pb = vec2(0.0, -0.25 * s), pc = vec2(0.5 * s, 0.25 * s);
        vec2 c1 = mc_closest(lc, pa, pb), c2 = mc_closest(lc, pb, pc);
        vec2 dv = lc - (length(lc - c1) < length(lc - c2) ? c1 : c2);
        float d = length(dv);
        vec2 g = d > 1e-5 ? dv / d : vec2(0.0, 1.0);
        return mc_cover(d - 0.11 * s, mc_aaAlong(g.x * ux + g.y * ur));
    }
    if (type >= 4 && type <= 6) {
        // polygon {n} or star {n/2}, {n/3}: chords between vertices on the row circle, the first at the top
        int nn = int(clamp(n, 2.0, 32.0) + 0.5);
        float stepA = float(type - 3) * MC_TAU / float(nn);
        float best = 1e9;
        vec2 bv = er;
        for (int j = 0; j < 32; j++) {
            if (j >= nn) break;
            float va = a0 + float(j) * MC_TAU / float(nn);
            vec2 dv = pl - mc_closest(pl, R * vec2(cos(va), sin(va)), R * vec2(cos(va + stepA), sin(va + stepA)));
            float d = length(dv);
            if (d < best) {
                best = d;
                bv = dv;
            }
        }
        return mc_cover(best - 0.5 * s, mc_aaAlong(best > 1e-5 ? bv / best : er));
    }
    if (type == 8) {
        // random arcs, one chance per slot: start, length and thickness from the seed
        float k = floor(phi), f = phi - k;
        vec2 hs = vec2(mod(k, n) + float(i) * 131.0, ta.p[8].w * 17.0);
        if (hash12(hs) > 0.8) return 0.0;
        float st = 0.04 + 0.3 * hash12(hs + 7.1);
        float len = 0.2 + (0.72 - st) * hash12(hs + 13.7);
        float thick = s * (0.35 + 0.65 * hash12(hs + 29.3));
        return mc_cover(abs(r - R) - 0.5 * thick, aaR) * mc_cover(max(st - f, f - st - len) * slot * r, aaT);
    }
    // hex grid filling the band [R - s, R], n cells around, fading inward
    float cs = MC_TAU * R / n;
    vec2 hp = abs(mc_hexCell(pl / cs).xy);
    float edge = max(dot(hp, vec2(0.5, 0.8660254)), hp.x);  // 0.5 on the cell outline
    float band = mc_cover(r - R, aaR) * smoothstep(R - s, R - 0.55 * s, r);
    return band * mc_cover((0.5 - edge) * cs - 0.5 * max(1.0, 0.08 * cs), 0.5 * (aaR + aaT));
}

vec4 gen_magicCircle() {
    vec2 center = ta.p[6].xy;
    vec2 spin = ta.p[6].zw;
    vec2 tilt = ta.p[7].xy;     // (sin, cos)
    float focal = ta.p[7].z;
    int rows = int(ta.p[7].w + 0.5);
    float radius = ta.p[8].x, fill = ta.p[8].y, fillR = ta.p[8].z;

    vec2 lp = gen_spacePos() - center;
    // floor plane laid back about the x axis, seen from `focal` in front of the centre (as the
    // pattern fill's): screen y = f v cos t / (f + v sin t), screen x = f u / (f + v sin t), inverted
    float den = focal * tilt.y - lp.y * tilt.x;
    float v = lp.y * focal / max(den, 1e-3 * focal);
    vec2 pl = vec2(lp.x * (focal + v * tilt.x) / focal, v);
    pl = vec2(spin.x * pl.x + spin.y * pl.y, -spin.y * pl.x + spin.x * pl.y);
    // the Jacobian of the tilted plane, for the anti-aliasing widths (target rows run down, which
    // flips mc_Jy against Pachislop's; only lengths are taken of it)
    mc_Jx = dFdx(pl);
    mc_Jy = dFdy(pl);
    float r = length(pl);
    vec2 er = r > 1e-4 ? pl / r : vec2(1.0, 0.0);
    float aaR = mc_aaAlong(er), aaT = mc_aaAlong(vec2(-er.y, er.x));
    float cov = 0.0;
    for (int i = 0; i < MC_MAX_ROWS; i++) {
        if (i >= rows) break;
        cov = max(cov, mc_rowCover(i, pl, r, er, aaR, aaT));
    }
    // inner fill: a disc to the outermost row, brightening toward its rim like a lit lens
    float fr = r / max(fillR, 1.0);
    cov = max(cov, fill * mc_cover(r - fillR, aaR) * (0.35 + 0.65 * min(fr * fr, 1.0)));
    // the ramp sampled as Pachislop's (u = r / radius, not at texel centres), decoded to linear
    vec4 g = texture(TA_TEX0, vec2(clamp(r / radius, 0.0, 1.0), 0.5));
    float a = cov * g.a * (den > 1e-3 * focal ? 1.0 : 0.0);
    return vec4(ta_toLinear(g.rgb) * a, a);
}

#endif // TITLEART_GEN_MAGIC_CIRCLE_GLSL
