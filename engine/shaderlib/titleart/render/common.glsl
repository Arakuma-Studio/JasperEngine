// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What every program of the renderer shares (titleart_render.frag includes it first): the one
// uniform block, the texture slots a program asks for with TA_USE_TEXn / TA_USE_RAWn, the colour
// transfer functions, the fallback encodings of the scalar and seed buffers
// (render/TitleArtCaps.h), and ta_dir().
//
// Addressing: every target is addressed in memory rows (gl_FragCoord.xy is (column, row), row 0
// the first memory row, on every backend). Node targets keep the canvas's image rows (row 0 at
// the top); the 2D title's element frames keep layer-space rows bottom first (row 0 at the
// frame's lowest y), so the ported y-up formulas of Pachislop's element passes run unchanged and
// only the place pass (and the CPU's mask upload) convert between the two (render/Title2DPass.cpp).
//
// Slots: u_taTexN are filterable (sampled with texture()), u_taRawN are read with texelFetch
// and declared unfilterable, so float formats without linear filtering bind to them.

#define TA_PARAMS 48

uniform u_fs_titleart {
    // p[0]: the target's size and its inverse; p[1]: the output's encoding (taEncodeScalar);
    // p[2..43]: the program's own; p[44..47]: the encodings of u_taRaw0..3
    vec4 p[TA_PARAMS];
} ta;

#define TA_TARGET_SIZE (ta.p[0].xy)
#define TA_OUT_ENC (ta.p[1])
#define TA_RAW_ENC(n) (ta.p[44 + (n)])

#ifdef TA_USE_TEX0
    uniform texture2D u_taTex0;
    uniform sampler u_taTex0_smp;
    #define TA_TEX0 sampler2D(u_taTex0, u_taTex0_smp)
#endif
#ifdef TA_USE_TEX1
    uniform texture2D u_taTex1;
    uniform sampler u_taTex1_smp;
    #define TA_TEX1 sampler2D(u_taTex1, u_taTex1_smp)
#endif
#ifdef TA_USE_TEX2
    uniform texture2D u_taTex2;
    uniform sampler u_taTex2_smp;
    #define TA_TEX2 sampler2D(u_taTex2, u_taTex2_smp)
#endif
#ifdef TA_USE_TEX3
    uniform texture2D u_taTex3;
    uniform sampler u_taTex3_smp;
    #define TA_TEX3 sampler2D(u_taTex3, u_taTex3_smp)
#endif
#ifdef TA_USE_TEX4
    uniform texture2D u_taTex4;
    uniform sampler u_taTex4_smp;
    #define TA_TEX4 sampler2D(u_taTex4, u_taTex4_smp)
#endif
#ifdef TA_USE_TEX5
    uniform texture2D u_taTex5;
    uniform sampler u_taTex5_smp;
    #define TA_TEX5 sampler2D(u_taTex5, u_taTex5_smp)
#endif
#ifdef TA_USE_TEX6
    uniform texture2D u_taTex6;
    uniform sampler u_taTex6_smp;
    #define TA_TEX6 sampler2D(u_taTex6, u_taTex6_smp)
#endif
#ifdef TA_USE_TEX7
    uniform texture2D u_taTex7;
    uniform sampler u_taTex7_smp;
    #define TA_TEX7 sampler2D(u_taTex7, u_taTex7_smp)
#endif
#ifdef TA_USE_RAW0
    uniform texture2D u_taRaw0;
    uniform sampler u_taRaw0_smp;
    #define TA_RAW0 sampler2D(u_taRaw0, u_taRaw0_smp)
#endif
#ifdef TA_USE_RAW1
    uniform texture2D u_taRaw1;
    uniform sampler u_taRaw1_smp;
    #define TA_RAW1 sampler2D(u_taRaw1, u_taRaw1_smp)
#endif
#ifdef TA_USE_RAW2
    uniform texture2D u_taRaw2;
    uniform sampler u_taRaw2_smp;
    #define TA_RAW2 sampler2D(u_taRaw2, u_taRaw2_smp)
#endif
#ifdef TA_USE_RAW3
    uniform texture2D u_taRaw3;
    uniform sampler u_taRaw3_smp;
    #define TA_RAW3 sampler2D(u_taRaw3, u_taRaw3_smp)
#endif
#ifdef TA_USE_RAW4
    uniform texture2D u_taRaw4;
    uniform sampler u_taRaw4_smp;
    #define TA_RAW4 sampler2D(u_taRaw4, u_taRaw4_smp)
#endif

out vec4 frag_color;

// ---- colour (sRGB transfer, clamped like Pachislop's toLinear / toSrgb) ----

vec3 taToLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 taToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
float taLuma709(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---- the fallback encodings of the scalar and seed buffers (TitleArtCaps.h TitleEncoding) ----
// An encoding descriptor: x 0 native (the value in .r; seeds in .xyz), 1 Fixed16 (a 16-bit
// unsigned in RG of an RGBA8 target, value = code / y - z), 2 Fixed12_4 (seeds: x and y as 12.4
// fixed point in RG and BA, 0xFFFF = no seed).

float taByte(float v) { return floor(v * 255.0 + 0.5); }

float taDecodeScalar(vec4 t, vec4 enc) {
    if (enc.x < 0.5) return t.r;
    float code = taByte(t.r) * 256.0 + taByte(t.g);
    return code / enc.y - enc.z;
}

vec4 taEncodeScalar(float v, vec4 enc) {
    if (enc.x < 0.5) return vec4(v, v, v, 1.0);
    float code = clamp(floor((v + enc.z) * enc.y + 0.5), 0.0, 65535.0);
    float hi = floor(code / 256.0);
    return vec4(hi / 255.0, (code - hi * 256.0) / 255.0, 0.0, 1.0);
}

// x, y: the seed's frame position (px); z: 1 when there is one
vec3 taDecodeSeed(vec4 t, vec4 enc) {
    if (enc.x < 1.5) return vec3(t.xy, t.z > 0.5 ? 1.0 : 0.0);
    float x = taByte(t.r) * 256.0 + taByte(t.g);
    float y = taByte(t.b) * 256.0 + taByte(t.a);
    if (x >= 65535.0) return vec3(0.0);
    return vec3(x / 16.0, y / 16.0, 1.0);
}

vec4 taEncodeSeed(vec2 pos, bool valid, vec4 enc) {
    if (enc.x < 1.5) return valid ? vec4(pos, 1.0, 0.0) : vec4(0.0);
    if (!valid) return vec4(1.0);
    vec2 code = clamp(floor(pos * 16.0 + 0.5), vec2(0.0), vec2(65534.0));
    vec2 hi = floor(code / 256.0);
    vec2 lo = code - hi * 256.0;
    return vec4(hi.x, lo.x, hi.y, lo.y) / 255.0;
}

// ---- directions ----
// The one place a Pachislop direction becomes a vector of a target (§6.3): `convention` 0 is
// clockwise on screen from +x (fills, shadows, rotations), 1 is Photoshop's light angle
// (counter-clockwise from +x, the direction the light comes from). `rowsDown` true gives the
// vector in rows-down frames (node targets: +y is the next memory row), false in y-up frames
// (the 2D title's element frames).
vec2 ta_dir(float angleDeg, int convention, bool rowsDown) {
    float a = radians(angleDeg);
    // y-up: clockwise on screen is a negative angle
    vec2 up = convention == 0 ? vec2(cos(a), -sin(a)) : vec2(cos(a), sin(a));
    return rowsDown ? vec2(up.x, -up.y) : up;
}
