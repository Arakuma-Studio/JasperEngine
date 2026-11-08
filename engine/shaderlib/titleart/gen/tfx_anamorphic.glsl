// Pachislop's anamorphic lens streak (src/generators/common.ts ANAMORPHIC_GLSL), ported by
// SafEight's permission, for the generators that draw one (Lens Flare, the Glints hero streak).
//
// anamorphicStreak(lv, size, len, col): a hot thin line plus a softer sheath along x, white at the
// centre and `col` outward. `lv` = the offset in the streak's frame, `size` = the core size (it
// sets the thickness), `len` = the falloff length along x; all in target px.

#ifndef TITLEART_GEN_TFX_ANAMORPHIC_GLSL
#define TITLEART_GEN_TFX_ANAMORPHIC_GLSL

vec3 anamorphicStreak(vec2 lv, float size, float len, vec3 col) {
    float sy = abs(lv.y) / size;
    float sx = abs(lv.x) / max(len, 1.0);
    float streak = exp(-sy * 28.0) * exp(-sx * 2.2) * 1.4 + exp(-sy * 7.0) * exp(-sx * 4.0) * 0.35;
    return mix(col, vec3(1.0), exp(-sx * 6.0) * exp(-sy * 40.0)) * streak;
}

#endif // TITLEART_GEN_TFX_ANAMORPHIC_GLSL
