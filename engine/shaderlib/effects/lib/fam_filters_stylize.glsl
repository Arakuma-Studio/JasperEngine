// HTML UI effects, filter family (p5-fx-filters): the stylize filters' fields. Ported from the
// shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): patternPaints.ts (the dither's periodic threshold fields,
// blue and white noise, ditherOrderedResult; the halftone plates) and motionBlur.ts (the CRT
// shades, the VHS tape geometry and YIQ recombine, the glitch geometry, fills and scanlines),
// with their constants. Angles are radians.
#ifndef FAM_FILTERS_STYLIZE_GLSL
#define FAM_FILTERS_STYLIZE_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"

// ---- dither (patternPaints.ts)

// ditherBayerQuad: y*3 + x*2 - x*y*4 for x, y in {0, 1}
float fam_dither_bayer_quad(float a, float b){
    return b * 3.0 + a * 2.0 - a * b * 4.0;
}

// ditherPeriodic: vec4(bayer2, bayer4, bayer8, clusteredDot) at a cell coordinate
vec4 fam_dither_periodic(vec2 coord){
    float tx = coord.x - 8.0 * floor(coord.x / 8.0);
    float ty = coord.y - 8.0 * floor(coord.y / 8.0);
    float x2 = tx - 2.0 * floor(tx / 2.0);
    float y2 = ty - 2.0 * floor(ty / 2.0);
    float x4 = tx - 4.0 * floor(tx / 4.0);
    float y4 = ty - 4.0 * floor(ty / 4.0);
    float x4h = floor(x4 / 2.0);
    float y4h = floor(y4 / 2.0);
    float x8h = floor(tx / 4.0);
    float y8h = floor(ty / 4.0);
    float b00 = fam_dither_bayer_quad(x2, y2);
    float b11 = fam_dither_bayer_quad(x4h, y4h);
    float b22 = fam_dither_bayer_quad(x8h, y8h);
    float bayer2 = b00 / 4.0;
    float bayer4 = (b00 * 4.0 + b11) / 16.0;
    float bayer8 = (b00 * 16.0 + b11 * 4.0 + b22) / 64.0;
    // clusteredDot = cM[y4][x4] / 16 as the kit's select ladder over the flat index
    int idx = int(y4) * 4 + int(x4);
    float cd = 0.75;
    cd = idx == 1 ? 0.3125 : cd;
    cd = idx == 2 ? 0.375 : cd;
    cd = idx == 3 ? 0.8125 : cd;
    cd = idx == 4 ? 0.25 : cd;
    cd = idx == 5 ? 0.0 : cd;
    cd = idx == 6 ? 0.0625 : cd;
    cd = idx == 7 ? 0.4375 : cd;
    cd = idx == 8 ? 0.6875 : cd;
    cd = idx == 9 ? 0.1875 : cd;
    cd = idx == 10 ? 0.125 : cd;
    cd = idx == 11 ? 0.5 : cd;
    cd = idx == 12 ? 0.9375 : cd;
    cd = idx == 13 ? 0.625 : cd;
    cd = idx == 14 ? 0.5625 : cd;
    cd = idx == 15 ? 0.875 : cd;
    return vec4(bayer2, bayer4, bayer8, cd);
}

// ditherBlueNoise: the interleaved-gradient field
float fam_dither_blue_noise(vec2 coord){
    vec2 c = coord + vec2(0.5);
    return fract(52.9829189 * fract(c.x * 0.06711056 + c.y * 0.00583715));
}

// ditherOrderedResult: the field contracted around 0.5 by spread, stepped against the luminance
float fam_dither_ordered_result(float ditherValue, float luminance, float threshold, float spread){
    float adjustedDither = 0.5 + (ditherValue - 0.5) * spread;
    return step(adjustedDither, luminance + (threshold - 0.5));
}

// ---- halftone (patternPaints.ts)

// the screen grid of a plate: the aspect-corrected uv turned by the screen angle, cell-local
vec2 fam_halftone_grid(vec2 uv, float aspect, float angle, float frequency){
    vec2 aspectUV = vec2(uv.x * aspect, uv.y);
    float c = cos(angle);
    float s = sin(angle);
    vec2 rotatedUV = vec2(dot(aspectUV, vec2(c, -s)), dot(aspectUV, vec2(s, c)));
    return fract(rotatedUV * frequency) - vec2(0.5);
}

// halftonePlateGrid: 1 inside the plate's dot, sized by its intensity
float fam_halftone_plate(vec2 uv, float aspect, float angle, float intensity, float frequency){
    const float SMOOTHNESS = 0.1;
    float dist = length(fam_halftone_grid(uv, aspect, angle, frequency));
    float size = intensity * 0.75 - SMOOTHNESS;
    return 1.0 - smoothstep(size - SMOOTHNESS, size + SMOOTHNESS, dist);
}

// halftoneChannelK/C/M/Y: the CMYK components of a colour (the black plate's K, the others'
// share of what K leaves)
vec4 fam_halftone_cmyk(vec3 s){
    float k = 1.0 - max(max(s.r, s.g), s.b);
    float invK = max(1.0 - k, 0.0001);
    return vec4((1.0 - s.r - k) / invK, (1.0 - s.g - k) / invK, (1.0 - s.b - k) / invK, k);
}

// halftoneTransmission: white fading to the ink where the dot covers (x the ink's alpha)
vec3 fam_halftone_transmission(vec4 ink, float inkMask){
    return mix(vec3(1.0), ink.rgb, vec3(inkMask * ink.a));
}

// ---- VHS (motionBlur.ts): value noise over the bit-cast hash, the tape geometry

float fam_vhs_smooth_noise(vec2 p){
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (vec2(3.0) - f * 2.0);
    float a = jfx_hash12(i);
    float b = jfx_hash12(i + vec2(1.0, 0.0));
    float c = jfx_hash12(i + vec2(0.0, 1.0));
    float d = jfx_hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// burstGate: slow scrolling noise, hard-edged
float fam_vhs_burst_gate(float t){
    return smoothstep(0.45, 0.8, fam_vhs_smooth_noise(vec2(t * 0.4, 0.0)));
}

// rowJitter: per-scanline (487 fields) hashes -> (luma, chroma) row offsets
vec2 fam_vhs_row_jitter(float uvY, float t, float scanlineNoise, float burst){
    float rowInt = floor(uvY * 487.0);
    float h0 = (jfx_hash12(vec2(rowInt, t)) - 0.5) * 2.0;
    float h1 = (jfx_hash12(vec2(rowInt, t + 69.42)) - 0.5) * 2.0;
    float fineGate = 0.25 + burst * 0.75;
    return vec2(h1 * scanlineNoise * 0.004 * fineGate, h0 * scanlineNoise * 0.008 * fineGate);
}

// tapeWave + creaseShift + headSwitch, folded into vhsSampleUVs: vec4(lumaUV, chromaUV)
vec4 fam_vhs_sample_uvs(vec2 uv, float t, float wobble, float scanlineNoise){
    float burst = fam_vhs_burst_gate(t);
    vec2 jitter = fam_vhs_row_jitter(uv.y, t, scanlineNoise, burst);
    // headSwitch (the top ~6 % of the frame)
    float switchPhase = 1.0 - smoothstep(0.0, 0.06, uv.y);
    float switchNoise = fam_vhs_smooth_noise(vec2(uv.y * 60.0, t * 14.0)) - 0.5;
    vec2 sw = vec2(switchPhase * switchNoise * wobble * 0.09, switchPhase * wobble * burst * 0.02);
    // tapeWave
    float wave1 = fam_vhs_smooth_noise(vec2(uv.y * 3.0, t * 0.8)) - 0.5;
    float wave2 = fam_vhs_smooth_noise(vec2(uv.y * 30.0, t * 6.0)) - 0.5;
    float tapeWave = (wave1 * 0.008 + wave2 * 0.002) * wobble;
    // creaseShift
    float creasePhase = smoothstep(0.92, 0.99, sin(uv.y * 8.0 - t * 3.77));
    float creaseNoise = smoothstep(0.3, 1.0, fam_vhs_smooth_noise(vec2(uv.y * 4.77, t)));
    float crease = creasePhase * creaseNoise * wobble * -0.018;
    float globalX = tapeWave + crease + sw.x;
    float sy = uv.y + sw.y;
    return vec4(uv.x + globalX + jitter.x, sy, uv.x + globalX + jitter.y, sy);
}

// yiqRecombine: sharp Y from the luma tap, I/Q from the five smeared chroma taps (weights i/15)
vec3 fam_vhs_yiq_recombine(vec3 luma, vec3 c1, vec3 c2, vec3 c3, vec3 c4, vec3 c5){
    float sharpY = dot(luma, vec3(0.299, 0.587, 0.114));
    vec3 iAxis = vec3(0.596, -0.274, -0.322);
    vec3 qAxis = vec3(0.211, -0.523, 0.312);
    float accI = dot(c1, iAxis) * 0.06666666666666667 + dot(c2, iAxis) * 0.13333333333333333 + dot(c3, iAxis) * 0.2
               + dot(c4, iAxis) * 0.26666666666666666 + dot(c5, iAxis) * 0.3333333333333333;
    float accQ = dot(c1, qAxis) * 0.06666666666666667 + dot(c2, qAxis) * 0.13333333333333333 + dot(c3, qAxis) * 0.2
               + dot(c4, qAxis) * 0.26666666666666666 + dot(c5, qAxis) * 0.3333333333333333;
    return vec3(sharpY + accI * 0.956 + accQ * 0.621, sharpY - accI * 0.272 - accQ * 0.647, sharpY - accI * 1.106 + accQ * 1.703);
}

// vhsAcBeat: the faint mains-hum brightness throb (t >= 0)
float fam_vhs_ac_beat(vec2 uv, float t, float wobble){
    return 1.0 + cos(jfx_fmod(t, 6.2831853) * 2.0 + uv.y * 0.5) * 0.015 * wobble;
}

// ---- glitch (motionBlur.ts)

// burstPulse: bursts of glitch followed by calm
float fam_glitch_burst_pulse(float slowFrame, float fastFrame, float intensity){
    float slowHash = jfx_hash11(slowFrame);
    float slowPulse = slowHash * slowHash * slowHash;
    float fastHash = jfx_hash11(fastFrame + 73.0);
    float fastPulse = fastHash * fastHash;
    float threshold = 1.0 - intensity;
    float majorBurst = step(threshold, slowPulse);
    float minorFlicker = step(threshold + 0.15, fastPulse);
    return clamp(majorBurst * 0.8 + minorFlicker * 0.4, 0.0, 1.0) * intensity;
}

// bandJitter: vec3(displace, active, seed) of one horizontal band
vec3 fam_glitch_band(float bandY, float frameIndex, float seedScale, float frameScale, float gateFactor, float hashOffset, float amplitude,
                     float strength){
    float seed = bandY * seedScale + frameIndex * frameScale;
    float bandHash = jfx_hash11(seed);
    float isActive = step(1.0 - strength * gateFactor, bandHash);
    float displace = (jfx_hash11(seed + hashOffset) - 0.5) * amplitude * isActive;
    return vec3(displace, isActive, seed);
}

// GlitchGeom
struct FamGlitchGeom {
    vec2 mirroredUV;
    float spread;
    float strength;
    float bandY1;
    float slowFrame;
    float bandGate;
    float blockGate;
    float distortion;
};

// glitchGeom: t = the integrated clock
FamGlitchGeom fam_glitch_geom(vec2 uv, float t, float intensity, float blockDensity, float mirrorChance){
    float slowFrame = floor(t * 2.5);
    float fastFrame = floor(t * 10.0);
    float strength = fam_glitch_burst_pulse(slowFrame, fastFrame, intensity);

    float bandY1 = floor(uv.y * blockDensity);
    vec3 band1 = fam_glitch_band(bandY1, slowFrame, 0.37, 7.13, 0.5, 42.0, 0.25, strength);
    float bandY2 = floor(uv.y * (blockDensity * 2.0));
    vec3 band2 = fam_glitch_band(bandY2, fastFrame, 0.53, 3.37, 0.7, 99.0, 0.12, strength);
    float bandY3 = floor(uv.y * (blockDensity * 4.0));
    vec3 band3 = fam_glitch_band(bandY3, fastFrame, 0.71, 11.71, 0.9, 173.0, 0.05, strength);

    // blockShift: coarse displacement on 4 columns
    float colX = floor(uv.x * 4.0);
    float blockSeed = colX * 0.41 + bandY1 * 0.67 + slowFrame * 2.31;
    float blockActive = step(1.0 - strength * 0.35, jfx_hash11(blockSeed));
    float blockDisplace = (jfx_hash11(blockSeed + 77.0) - 0.5) * 0.18 * blockActive;

    float totalDisplaceX = band1.x + band2.x + band3.x + blockDisplace;
    float vertJitter = band1.y * (jfx_hash11(band1.z + 200.0) - 0.5) * 0.012;
    vec2 displacedUV = vec2(clamp(uv.x + totalDisplaceX * strength, 0.0, 1.0), clamp(uv.y + vertJitter * strength, 0.0, 1.0));

    // mirrorFlips
    float mirrorSeed = colX * 0.83 + bandY1 * 1.17 + slowFrame * 4.57;
    float hMirrorActive = step(1.0 - mirrorChance * strength, jfx_hash11(mirrorSeed)) * band1.y;
    float vMirrorActive = step(1.0 - mirrorChance * strength * 0.4, jfx_hash11(mirrorSeed + 500.0)) * band1.y;

    FamGlitchGeom g;
    g.mirroredUV = vec2(mix(displacedUV.x, 1.0 - displacedUV.x, hMirrorActive), mix(displacedUV.y, 1.0 - displacedUV.y, vMirrorActive));
    g.spread = strength + abs(totalDisplaceX) * 2.0;
    g.strength = strength;
    g.bandY1 = bandY1;
    g.slowFrame = slowFrame;
    g.bandGate = band1.y;
    g.blockGate = blockActive;
    g.distortion = clamp(band1.y + band2.y + band3.y, 0.0, 1.0);
    return g;
}

// fillBarsShade: SMPTE bars or a solid neon hue in the active blocks, on premultiplied colour
vec4 fam_glitch_fill_bars(vec4 color, vec2 uv, FamGlitchGeom g, float colorBarMix){
    float colX = floor(uv.x * 4.0);
    float blockSeed = colX * 0.41 + g.bandY1 * 0.67 + g.slowFrame * 2.31;
    float fillStyleHash = jfx_hash11(blockSeed + 555.0);
    float barOffset = jfx_hash11(g.bandY1 + g.slowFrame * 3.33 + 333.0);
    float barX = fract(uv.x * 3.0 + barOffset * 7.0);
    float barIndex = floor(barX * 7.0);
    float barR = (1.0 - step(2.0, barIndex)) + step(4.0, barIndex) * (1.0 - step(6.0, barIndex));
    float barG = 1.0 - step(4.0, barIndex);
    float barB = 1.0 - step(0.5, fract(barIndex * 0.5));
    vec3 smpteColor = vec3(barR, barG, barB);
    float solidHue = fract(jfx_hash11(g.bandY1 + 13.0) * 0.8 + g.slowFrame * 0.07);
    float sh6 = solidHue * 6.0;
    vec3 solidColor = vec3(clamp(abs(sh6 - 3.0) - 1.0, 0.0, 1.0), clamp(2.0 - abs(sh6 - 2.0), 0.0, 1.0), clamp(2.0 - abs(sh6 - 4.0), 0.0, 1.0)) * 1.3;
    vec3 fillColor = mix(solidColor, smpteColor, vec3(step(0.5, fillStyleHash)));
    float colorBarStrength = g.bandGate * g.blockGate * colorBarMix * g.strength;
    return vec4(mix(color.rgb, fillColor * color.a, vec3(colorBarStrength)), color.a);
}

#endif // FAM_FILTERS_STYLIZE_GLSL
