// Title Art: what every program of titleart.frag shares (docs/specs/S80-title-art-core.md §6.3).
//
// The pixel contract: intermediates hold linear, premultiplied colour (RGBA16F, or SRGB8A8 on
// the RGBA8 path, whose hardware encode and decode keep the same values in the shader).
// Frame space is memory rows: gl_FragCoord.xy is (column, row) with row 0 the first memory row,
// the top of the image, so +y points down the screen. A direction given clockwise on screen
// from +x is (cos, sin) here, which the C++ side computes (TitleFx: dirCw) before upload.
//
// The block (TitlePass::params on the C++ side, render/TitlePasses.h):
//   p[0] = (target width, height, px_scale, 0)
//   p[1] = the whole render (the canvas at px_scale) in texture coordinates of the target: x0, y0,
//          x1, y1 (the same in every pyramid level). Light past it is cut there; Repeat Edge
//          Pixels clamps to it.
//   p[2..15] = the program's own values.
// Textures: u_taTex0..3 are sampled filtered (colour, coverage, LUTs); u_taRaw0..1 are read with
// texelFetch through non-filtering samplers (jump-flood seeds, distances, the bounds reduction:
// formats a device may not filter; titleart.frag carries their sokol annotations).

#ifndef TITLEART_COMMON_GLSL
#define TITLEART_COMMON_GLSL

uniform u_fs_titleart {
    vec4 p[16];
} ta;

uniform texture2D u_taTex0;
uniform sampler u_taTex0_smp;
uniform texture2D u_taTex1;
uniform sampler u_taTex1_smp;
uniform texture2D u_taTex2;
uniform sampler u_taTex2_smp;
uniform texture2D u_taTex3;
uniform sampler u_taTex3_smp;
uniform texture2D u_taRaw0;
uniform sampler u_taRaw0_smp;
uniform texture2D u_taRaw1;
uniform sampler u_taRaw1_smp;

#define TA_TEX0 sampler2D(u_taTex0, u_taTex0_smp)
#define TA_TEX1 sampler2D(u_taTex1, u_taTex1_smp)
#define TA_TEX2 sampler2D(u_taTex2, u_taTex2_smp)
#define TA_TEX3 sampler2D(u_taTex3, u_taTex3_smp)
#define TA_RAW0 sampler2D(u_taRaw0, u_taRaw0_smp)
#define TA_RAW1 sampler2D(u_taRaw1, u_taRaw1_smp)

#define TA_SIZE (ta.p[0].xy)
#define TA_PX (ta.p[0].z)
#define TA_FRAME (ta.p[1])

const float TA_PI = 3.14159265359;

// the pixel centre's texture coordinate in a target of the pass's size (also the coordinate of
// the same place in any smaller pyramid level)
vec2 ta_uv() {
    return gl_FragCoord.xy / TA_SIZE;
}

// the whole render in uv of the target: x0, y0, x1, y1
vec4 ta_frameUv() {
    return TA_FRAME;
}

vec3 ta_unpremul(vec4 c) {
    return c.a > 1e-5 ? c.rgb / c.a : vec3(0.0);
}

vec3 ta_toSrgb(vec3 c) {
    c = max(c, 0.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 ta_toLinear(vec3 c) {
    c = max(c, 0.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// Rec.709 luminance of linear colour
float ta_luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// the texture coordinate of t (0..1) in a 256-entry lookup sampled at its texel centres
// (gradient ramps, tone curves)
vec2 ta_lutUv(float t) {
    return vec2(clamp(t, 0.0, 1.0) * (255.0 / 256.0) + 0.5 / 256.0, 0.5);
}

// ---- the RGBA8 fallback encodings (TitleArtCaps, §6.3) ----

// two bytes of an RGBA8 texel (read as 0..1) as one 16-bit value
float ta_bytes16(float hi, float lo) {
    return floor(hi * 255.0 + 0.5) * 256.0 + floor(lo * 255.0 + 0.5);
}

// one 16-bit value 0..65535 as two bytes for an RGBA8 write
vec2 ta_split16(float v) {
    float x = clamp(floor(v + 0.5), 0.0, 65535.0);
    float hi = floor(x / 256.0);
    return vec2(hi, x - hi * 256.0) / 255.0;
}

#endif // TITLEART_COMMON_GLSL
