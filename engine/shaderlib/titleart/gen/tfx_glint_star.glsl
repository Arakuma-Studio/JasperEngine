// The glint star (Pachislop src/generators/common.ts GLINT_STAR_GLSL), ported by SafEight's
// permission: the Glints sprites and the particles' 4- and 6-point glints. A glint as light: an
// overexposed white core (well past 1, burns out under Add) in a soft round bloom, and rays that
// start thick and hot at the core and fall off fast into hair-thin, faint tips. Colour runs from
// white at the core to `col`, more saturated, at the tips; `spectral` fans each ray out by
// wavelength (red reaches furthest and widest, blue least) and rings the bloom with a faint
// rainbow.
//
// glintStar(v, rays, thin, minThin, px, col, spectral): `v` the position in star radii (the
// star's frame), `rays` 4 (a cross plus faint short diagonals), 6 (snow cross: three needle axes)
// or 8 (4 long + 4 short), `thin` the ray half-width at the core in star radii, `minThin` the
// smallest core half-width (Pachislop's GLINT_MIN_THIN, 0.6 canvas px, in star radii; the tips
// keep 60 % of it), `px` one target pixel in star radii (rays never get thinner than about half
// a pixel: a sub-pixel tip keeps its light spread over the pixel, so the look holds at every
// px_scale). Self-contained: no block, texture or other include.

#ifndef TITLEART_GEN_TFX_GLINT_STAR_GLSL
#define TITLEART_GEN_TFX_GLINT_STAR_GLSL

// One tapered ray along d (unit) reaching len radii: intensity at v. Its half-width runs from
// thin at the core down to 12 %, but not below minW.
float glintRay(vec2 v, vec2 d, float len, float thin, float minW, float px) {
    float al = abs(dot(v, d)) / len;
    if (al >= 1.0) return 0.0;
    float pe = abs(dot(v, vec2(-d.y, d.x)));
    float u = 1.0 - al;
    float w = max(thin * (0.12 + 0.88 * u * u), minW);
    float we = sqrt(w * w + 0.25 * px * px);
    // past ~30 widths both profiles are below 1e-4 of the ray: skip the exponentials
    if (pe > 30.0 * we) return 0.0;
    // spike (crisp; under a pixel it keeps its light, spread over the pixel) + a soft sheath that
    // dies off near the core; one exp each across (spike = sheath profile^4) and along (e^-al powers)
    float sh = exp(-pe / (we * 4.0));
    float sp = sh * sh;
    float e1 = exp(-al);
    float e2 = e1 * e1;
    float spike = sp * sp * (w / we);
    float sheath = 0.22 * sh * e2 * e1;
    // hot near the core, a long faint exponential tail, reaching 0 at the tip
    float fall = (e2 * e2 * e1 * 2.0 + 1.3 * e1 * sqrt(e1)) * sqrt(u);
    return (spike + sheath) * fall;
}

vec3 glintSpectrum(float t) {
    return clamp(abs(fract(t + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

// All rays of the shape for one wavelength: len and width scaled by ls, ws.
float glintRays(vec2 v, int rays, float thin, float minW, float px, float ls, float ws) {
    float t = thin * ws;
    if (rays == 6) {
        t *= 0.7;
        return glintRay(v, vec2(0.0, 1.0), ls, t, minW, px) + glintRay(v, vec2(0.866025, 0.5), ls, t, minW, px) +
               glintRay(v, vec2(-0.866025, 0.5), ls, t, minW, px);
    }
    float I = glintRay(v, vec2(1.0, 0.0), ls, t, minW, px) + glintRay(v, vec2(0.0, 1.0), ls, t, minW, px);
    // 8: four short diagonals at 60 %; 4: a faint short diagonal pair, the secondary set of a star filter
    float dk = rays == 8 ? 0.6 : 0.16;
    float dl = rays == 8 ? 0.5 : 0.3;
    return I + dk * (glintRay(v, vec2(0.707107, 0.707107), ls * dl, t * 0.8, minW, px) +
                     glintRay(v, vec2(0.707107, -0.707107), ls * dl, t * 0.8, minW, px));
}

// minThin: the smallest core half-width (GLINT_MIN_THIN canvas px, in star radii); the tips keep 60 % of it.
vec3 glintStar(vec2 v, int rays, float thin, float minThin, float px, vec3 col, bool spectral) {
    float r = length(v);
    if (r >= 1.0) return vec3(0.0);
    thin = max(thin, minThin);
    float minW = 0.6 * minThin;
    // source colour, more saturated, toward the tips; white at the core
    float m = max(max(col.r, col.g), max(col.b, 1e-4));
    vec3 hue = col / m;
    vec3 sat = m * clamp(mix(vec3(dot(hue, vec3(0.2126, 0.7152, 0.0722))), hue, 1.4), 0.0, 1.0);
    float tip = smoothstep(0.08, 0.9, r);
    vec3 rayCol = mix(vec3(m), sat, tip);
    vec3 I;
    if (spectral) {
        I = vec3(glintRays(v, rays, thin, minW, px, 1.0, 1.3), glintRays(v, rays, thin, minW, px, 0.93, 1.0),
                 glintRays(v, rays, thin, minW, px, 0.84, 0.75));
        // diffraction: the ray runs white -> violet ... red outward
        rayCol *= mix(vec3(1.0), 1.6 * glintSpectrum(0.75 * (1.0 - smoothstep(0.1, 0.95, r))), smoothstep(0.08, 0.35, r));
    } else {
        I = vec3(glintRays(v, rays, thin, minW, px, 1.0, 1.0));
    }
    float edge = 1.0 - smoothstep(0.55, 1.0, r);
    // round bloom: a tight glow on the core plus a wide faint skirt
    float bloom = (0.8 * exp(-r * r * 45.0) + 0.35 * exp(-r * 5.0)) * edge;
    vec3 bloomCol = mix(vec3(m), sat, smoothstep(0.0, 0.25, r));
    // Pachislop's pow((r - 0.24) / 0.07, 2.0) written as a square: GLSL's pow is undefined for a
    // negative base (NaN on some backends) and the ring is meant to be a Gaussian either side
    float ringD = (r - 0.24) / 0.07;
    if (spectral) bloomCol += 0.5 * m * glintSpectrum(0.72 - 2.0 * r) * exp(-ringD * ringD);
    // overexposed core: well past 1, white
    float rc = 0.04 + thin;
    float core = 4.0 * exp(-r * r / (rc * rc));
    return rayCol * I + bloomCol * bloom + vec3(m) * core;
}

#endif // TITLEART_GEN_TFX_GLINT_STAR_GLSL
