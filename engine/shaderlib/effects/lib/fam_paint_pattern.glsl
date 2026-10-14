// HTML UI effects, paint family (p5-fx-paint): lattice and stroke helpers of the pattern
// effects. Ported from the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): cells.ts (cellHash, variationFactor, rowSpeedHash,
// rowSpeedMultiplier), aa.ts (quilezLineFilterAxis, lineMaskFromField, discCoverage).
#ifndef FAM_PAINT_PATTERN_GLSL
#define FAM_PAINT_PATTERN_GLSL

// WGSL's smoothstep with equal edges is a step (the divide gives +-inf, clamped); GLSL leaves it
// undefined, so the ports that pass equal edges (softness 0) use this
float fam_smoothstep(float edge0, float edge1, float x){
    if (edge1 > edge0)
        return smoothstep(edge0, edge1, x);
    return x > edge0 ? 1.0 : 0.0;
}

// cells.ts cellHash: a hash of an integer cell id in [0, 1)
float fam_cell_hash(vec2 id){
    vec2 q0 = fract(id * vec2(123.34, 345.45));
    vec2 q1 = q0 + dot(q0, q0 + 34.345);
    return fract(q1.x * q1.y);
}

// cells.ts variationFactor of the cell's hash: the per-cell lighten/darken factor
float fam_cell_variation(vec2 cellId, float variation){
    return max(1.0 + (fam_cell_hash(cellId) - 0.5) * 2.0 * variation, 0.0);
}

// cells.ts rowSpeedHash and rowSpeedMultiplier
float fam_row_speed(float row, float variance){
    float rowHash = fract(sin(row * 127.1) * 43758.5453);
    return 1.0 + (rowHash - 0.5) * 4.0 * variance;
}

// aa.ts quilezLineFilterAxis: the share of a footprint w around p that falls in the line region
// [0, 1/N) of each cell
float fam_quilez_line_axis(float p, float w, float N){
    float a = p + w * 0.5;
    float b = p - w * 0.5;
    return (floor(a) + min(fract(a) * N, 1.0) - floor(b) - min(fract(b) * N, 1.0)) / (N * w);
}

// aa.ts lineMaskFromField: 1 inside a stroke band, 0 when the line width is 0
float fam_line_mask(float field, float lineWidth, float lo, float hi){
    return (1.0 - fam_smoothstep(lo, hi, field)) * step(0.0001, lineWidth);
}

// aa.ts discCoverage
float fam_disc_coverage(float dist, float radius, float footprint){
    return 1.0 - fam_smoothstep(radius - footprint * 0.5, radius, dist);
}

#endif // FAM_PAINT_PATTERN_GLSL
