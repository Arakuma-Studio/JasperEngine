// Palettes in the shader library (docs/specs/S43-palettes.md R12-R16, §6.5).
//
// Palette Swap and Palette Cycling read a material's `// @palette u_palette` binding: two RGBA8
// textures read with texelFetch only (no filtering), so every backend compares the same bytes
// (engine/core/pixelart/PaletteSwap.h lays them out; PaletteSwapData::swap is the CPU twin):
// - u_paletteCells (256 x 128): the 32^3 grid of 5-bit colours, each channel a candidate source
//   index + 1 (0: none);
// - u_paletteRows (256 x 17+): row 0 the source's colours, rows 1-15 the targets, row 16 the
//   cycle table, rows 17... each cycling ramp's indices, its speed at x = 255.
// The texel is the raw sRGB-encoded byte colour, so a match is exact and anything else
// (anti-aliased, filtered, off-palette) is left as it is.
//
// paletteSrgbToOklab is the GLSL twin of pixelart::toOklab, for diagnostics; the runtime passes
// need no colour space (the matches are bytes, Palette Quantize's choices are baked on the CPU).

#ifndef PALETTE_GLSL
#define PALETTE_GLSL

#if defined(PALETTE_SWAP) || defined(PALETTE_CYCLE)

uniform texture2D u_paletteCells;
uniform texture2D u_paletteRows;
uniform sampler u_palette_smp;

ivec4 paletteRowTexel(int x, int y){
    return ivec4(texelFetch(sampler2D(u_paletteRows, u_palette_smp), ivec2(x, y), 0) * 255.0 + 0.5);
}

// the source index of a colour given as bytes, or -1: the first of the cell's candidates whose
// colour is exactly these bytes
int paletteSourceIndex(ivec3 c){
    ivec3 cell = c / 8;
    ivec4 candidates = ivec4(texelFetch(sampler2D(u_paletteCells, u_palette_smp),
                                        ivec2(cell.r + 32 * (cell.b % 8), cell.g + 32 * (cell.b / 8)), 0) * 255.0 + 0.5);
    int index = -1;
    for (int k = 0; k < 4; k++){
        int candidate = candidates[k] - 1;
        if (index < 0 && candidate >= 0 && all(equal(paletteRowTexel(candidate, 0).rgb, c)))
            index = candidate;
    }
    return index;
}

#ifdef PALETTE_CYCLE
    // DPaint colour cycling: an index at position p of a cycling ramp of length L shows ramp
    // position (p + floor(time * speed * cycleSpeed)) mod L
    int paletteCycled(int index, float time, float cycleSpeed){
        ivec4 entry = paletteRowTexel(index, 16);
        if (entry.r == 0)
            return index;
        int orderRow = 17 + entry.b;
        ivec4 rate = paletteRowTexel(255, orderRow);
        float stepsPerSecond = float(rate.r * 256 + rate.g) / 256.0 - 128.0;
        float shifted = float(entry.r - 1) + floor(time * stepsPerSecond * cycleSpeed);
        int position = clamp(int(mod(shifted, float(entry.g))), 0, entry.g - 1);
        return paletteRowTexel(position, orderRow).r;
    }
#endif

// A straight-alpha, sRGB-encoded texel swapped to `row` (0: the source's own colours); its
// alpha times the target's, so a target alpha of 0 hides a part
vec4 paletteSwapTexel(vec4 texel, float row, float time, float cycleSpeed){
    ivec3 c = ivec3(clamp(texel.rgb, 0.0, 1.0) * 255.0 + 0.5);
    int index = paletteSourceIndex(c);
    if (index < 0)
        return texel;
    #ifdef PALETTE_CYCLE
        index = paletteCycled(index, time, cycleSpeed);
    #endif
    vec4 target = texelFetch(sampler2D(u_paletteRows, u_palette_smp), ivec2(index, clamp(int(row + 0.5), 0, 15)), 0);
    return vec4(target.rgb, texel.a * target.a);
}

#endif

// OKLab of a colour in sRGB encoding (the piecewise curve; Ottosson's matrices)
vec3 paletteSrgbToOklab(vec3 srgb){
    vec3 c = clamp(srgb, 0.0, 1.0);
    vec3 linear = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    vec3 lms = vec3(dot(vec3(0.4122214708, 0.5363325363, 0.0514459929), linear),
                    dot(vec3(0.2119034982, 0.6806995451, 0.1073969566), linear),
                    dot(vec3(0.0883024619, 0.2817188376, 0.6299787005), linear));
    lms = pow(max(lms, vec3(0.0)), vec3(1.0 / 3.0));
    return vec3(dot(vec3(0.2104542553, 0.7936177850, -0.0040720468), lms),
                dot(vec3(1.9779984951, -2.4285922050, 0.4505937099), lms),
                dot(vec3(0.0259040371, 0.7827717662, -0.8086757660), lms));
}

#endif
