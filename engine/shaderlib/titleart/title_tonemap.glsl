// The per-layer tone maps of 3D titles (docs/specs/S86-title-3d.md R33): ACES Filmic and AgX as
// three.js writes them (src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js,
// MIT, Copyright 2010-2025 three.js authors), Khronos PBR Neutral (Apache-2.0 sample code, as
// util/PostColorLut.cpp) and Punch, ported from Pachislop src/engine/three/tonemap.ts by
// SafEight, used with the author's permission. They run on straight, exposed colour (the
// caller divides by alpha and re-premultiplies).
//
// The file is valid C++ over a shim of the GLSL types it uses (TitleToneMapTest compiles it):
// keep it free of GLSL-only syntax (no qualifiers, no matrices, no swizzles).

// m = columns c0, c1, c2 (GLSL's mat3(c0, c1, c2) * v)
vec3 tl_mul3(vec3 c0, vec3 c1, vec3 c2, vec3 v) {
    return c0 * v.x + c1 * v.y + c2 * v.z;
}

vec3 tl_rrtAndOdtFit(vec3 v) {
    vec3 a = v * (v + vec3(0.0245786)) - vec3(0.000090537);
    vec3 b = v * (v * 0.983729 + vec3(0.4329510)) + vec3(0.238081);
    return a / b;
}

// three.js ACESFilmicToneMapping (Stephen Hill's fit, brightened by 1 / 0.6)
vec3 tl_acesFilmic(vec3 color) {
    color = color * (1.0 / 0.6);
    color = tl_mul3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777), color);
    color = tl_rrtAndOdtFit(color);
    color = tl_mul3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602), color);
    return clamp(color, vec3(0.0), vec3(1.0));
}

vec3 tl_agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return x4 * x2 * 15.5 - x4 * x * 40.14 + x4 * 31.96 - x2 * x * 6.868 + x2 * 0.4298 + x * 0.1191 - vec3(0.00232);
}

// three.js AgXToneMapping (Filament's, Blender's AgX on Rec.2020 primaries)
vec3 tl_agx(vec3 color) {
    const float AgxMinEv = -12.47393;
    const float AgxMaxEv = 4.026069;
    color = tl_mul3(vec3(0.6274, 0.0691, 0.0164), vec3(0.3293, 0.9195, 0.0880), vec3(0.0433, 0.0113, 0.8956), color);
    color = tl_mul3(vec3(0.856627153315983, 0.137318972929847, 0.11189821299995),
                    vec3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
                    vec3(0.0482516061458583, 0.101439036467562, 0.811302368396859), color);
    color = max(color, vec3(1e-10));
    color = log2(color);
    color = (color - vec3(AgxMinEv)) / (AgxMaxEv - AgxMinEv);
    color = clamp(color, vec3(0.0), vec3(1.0));
    color = tl_agxContrast(color);
    color = tl_mul3(vec3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
                    vec3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
                    vec3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405), color);
    color = pow(max(vec3(0.0), color), vec3(2.2));
    color = tl_mul3(vec3(1.6605, -0.1246, -0.0182), vec3(-0.5876, 1.1329, -0.1006), vec3(-0.0728, -0.0083, 1.1187), color);
    return clamp(color, vec3(0.0), vec3(1.0));
}

// Khronos PBR Neutral
vec3 tl_neutral(vec3 color) {
    const float StartCompression = 0.8 - 0.04;
    const float Desaturation = 0.15;
    float x = min(color.x, min(color.y, color.z));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color = color - vec3(offset);
    float peak = max(color.x, max(color.y, color.z));
    if (peak < StartCompression) return color;
    float d = 1.0 - StartCompression;
    float newPeak = 1.0 - d * d / (peak + d - StartCompression);
    color = color * (newPeak / peak);
    float g = 1.0 - 1.0 / (Desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// Punch: Neutral's toe and shoulder on the brightest channel with every channel scaled by the
// same factor (hue and saturation kept), whitening only the hottest light (peak 3 to 16)
vec3 tl_punch(vec3 color) {
    const float Start = 0.8 - 0.04;
    const float HotFrom = 3.0;
    const float HotTo = 16.0;
    float x = min(color.x, min(color.y, color.z));
    color = color - vec3(x < 0.08 ? x - 6.25 * x * x : 0.04);
    float peak = max(color.x, max(color.y, color.z));
    if (peak < Start) return color;
    float d = 1.0 - Start;
    float newPeak = 1.0 - d * d / (peak + d - Start);
    color = color * (newPeak / peak);
    return mix(color, vec3(newPeak), smoothstep(HotFrom, HotTo, peak));
}

// mode: 0 none, 1 ACES Filmic, 2 AgX, 3 Neutral, 4 Punch (ToneMapping3D)
vec3 tl_toneMap(vec3 color, int mode) {
    if (mode == 1) return tl_acesFilmic(color);
    if (mode == 2) return tl_agx(color);
    if (mode == 3) return tl_neutral(color);
    if (mode == 4) return tl_punch(color);
    return color;
}
