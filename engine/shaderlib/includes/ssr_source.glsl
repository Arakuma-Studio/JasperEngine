// The reflection source of a G-buffer pixel (color[2].a, an 8-bit code written by the SSR
// G-buffer pass): what its colour pass added as environment specular, which the SSR composite
// swaps for the screen-space reflection. Mirrors engine/core/render/SSRProbeSlots.h.
//
//   0                   Skip: no reflection to swap or add to (unlit, planar mirrors)
//   1                   Add: SSR is added, the environment is unknown or none
//   2                   Ambient: the scene ambient's specular (Lit without IBL)
//   3                   Sky: the sky's prefiltered map (Lit with IBL, outside every probe)
//   4                   a local probe without a composite slot: left as drawn (no SSR)
//   5 + 62 s + (q - 1)  the local probe in slot s (0..3), blended over the sky by q / 62
//   253..255            unused (treated as Skip)

#ifndef SSR_SOURCE_GLSL
#define SSR_SOURCE_GLSL

#define SSR_PROBE_SLOTS 4
#define SSR_PROBE_WEIGHT_STEPS 62

const int SSR_SOURCE_SKIP = 0;
const int SSR_SOURCE_ADD = 1;
const int SSR_SOURCE_AMBIENT = 2;
const int SSR_SOURCE_SKY = 3;
const int SSR_SOURCE_PROBE_NO_SLOT = 4;
const int SSR_SOURCE_PROBE_BASE = 5;
const int SSR_SOURCE_PROBE_END = SSR_SOURCE_PROBE_BASE + SSR_PROBE_SLOTS * SSR_PROBE_WEIGHT_STEPS;

int ssrSourceCode(float alpha){
    return int(alpha * 255.0 + 0.5);
}

// the composite leaves the pixel as the colour pass drew it (and the march skips it)
bool ssrSourceSkips(int code){
    return code == SSR_SOURCE_SKIP || code == SSR_SOURCE_PROBE_NO_SLOT || code >= SSR_SOURCE_PROBE_END;
}

bool ssrSourceIsProbe(int code){
    return code >= SSR_SOURCE_PROBE_BASE && code < SSR_SOURCE_PROBE_END;
}

int ssrSourceProbeSlot(int code){
    return (code - SSR_SOURCE_PROBE_BASE) / SSR_PROBE_WEIGHT_STEPS;
}

float ssrSourceProbeWeight(int code){
    int step = (code - SSR_SOURCE_PROBE_BASE) - ssrSourceProbeSlot(code) * SSR_PROBE_WEIGHT_STEPS + 1;
    return float(step) / float(SSR_PROBE_WEIGHT_STEPS);
}

#endif
