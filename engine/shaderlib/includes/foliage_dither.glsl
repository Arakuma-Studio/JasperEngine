// The fragment side of foliage's fade (T5 F6): a screen-space pattern against the factor the vertex
// stage computed from instance distance, so a band chosen per cell shows no square ring. No blending
// (it would need sorting) and no alpha to coverage (not on every target).

#ifndef FOLIAGE_DITHER_GLSL
#define FOLIAGE_DITHER_GLSL

#ifdef USE_FOLIAGE_DITHER

in float v_foliageFade;

// interleaved gradient noise (Jimenez 2014): one float per pixel, no texture
void foliageDitherDiscard(){
    if (v_foliageFade >= 1.0) return;
    float pattern = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))));
    if (v_foliageFade <= pattern) discard;
}

#endif

#endif
