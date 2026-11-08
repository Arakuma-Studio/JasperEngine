// The pixel-art family's one ordered-dither table (docs/specs/S43-palettes.md R21; S48 and S49
// include it and add no table of their own): the standard recursive Bayer matrices of size 2, 4
// and 8 (the same as jfx_bayer* and pixelfx's (v + 0.5) / 16 table), each giving (m + 0.5) / n^2
// for an integer cell (non-negative: GLSL ES leaves % of a negative undefined). The CPU twin is pixelart::bayerThreshold (engine/core/pixelart/PixelColor.h).
// A mix of ratio k / n^2 shows its second colour where the threshold is below the ratio.

#ifndef BAYER_GLSL
#define BAYER_GLSL

// M(2n) = [4 M + 0, 4 M + 2; 4 M + 3, 4 M + 1], one level per bit of the cell
int bayerLevel(ivec2 bit){
    return bit.y == 0 ? (bit.x == 0 ? 0 : 2) : (bit.x == 0 ? 3 : 1);
}

float bayer2(ivec2 cell){
    ivec2 p = ivec2(cell.x % 2, cell.y % 2);
    return (float(bayerLevel(p)) + 0.5) / 4.0;
}

float bayer4(ivec2 cell){
    ivec2 p = ivec2(cell.x % 4, cell.y % 4);
    int m = 4 * bayerLevel(p % 2) + bayerLevel(p / 2);
    return (float(m) + 0.5) / 16.0;
}

float bayer8(ivec2 cell){
    ivec2 p = ivec2(cell.x % 8, cell.y % 8);
    int m = 16 * bayerLevel(p % 2) + 4 * bayerLevel((p / 2) % 2) + bayerLevel(p / 4);
    return (float(m) + 0.5) / 64.0;
}

// the threshold of a dither of size 2, 4 or 8 (anything else: 4)
float bayerThreshold(int size, ivec2 cell){
    if (size == 2)
        return bayer2(cell);
    if (size == 8)
        return bayer8(cell);
    return bayer4(cell);
}

#endif
