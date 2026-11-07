// What every generator program shares (docs/specs/S83-title-effects-generators.md R9, R10),
// after titleart/common.glsl (the block u_fs_titleart and the texture slots).
//
// The block (generators/TitleGen.h genPass): p[2] target px -> drawing space (a, b, c, d), p[3] =
// (its tx, ty, target px per drawing-space px, px_scale), p[4] drawing space -> target px
// (a, b, c, d), p[5] = (its tx, ty, min_px, 0); p[6..15] the program's own. The drawing space is
// canvas px with +y up (Pachislop's layer space, or document space while following a layer).
// Targets are addressed in memory rows (row 0 at the top) like every title pass; textures of
// the target's size are sampled at gl_FragCoord.xy / TA_SIZE.

#ifndef TITLEART_GEN_COMMON_GLSL
#define TITLEART_GEN_COMMON_GLSL

// this fragment in the drawing space (Pachislop's layerPos())
vec2 gen_spacePos() {
    vec2 f = gl_FragCoord.xy;
    return vec2(ta.p[2].x * f.x + ta.p[2].y * f.y, ta.p[2].z * f.x + ta.p[2].w * f.y) + ta.p[3].xy;
}

// a drawing-space point in target px
vec2 gen_toTarget(vec2 s) {
    return vec2(ta.p[4].x * s.x + ta.p[4].y * s.y, ta.p[4].z * s.x + ta.p[4].w * s.y) + ta.p[5].xy;
}

// target px per drawing-space px (Pachislop's layerPxScale())
float gen_spaceScale() {
    return ta.p[3].z;
}

// a drawing-space direction (y up) in target px (rows down)
vec2 gen_dirToTarget(vec2 d) {
    return vec2(ta.p[4].x * d.x + ta.p[4].y * d.y, ta.p[4].z * d.x + ta.p[4].w * d.y);
}

// Pachislop's light-like colour as premultiplied rgba with alpha = the brightest channel
// (effects/b/common.ts lightPremul): light keeps its energy above 1 and unions coverage
vec4 gen_lightPremul(vec3 light) {
    light = max(light, 0.0);
    float la = clamp(max(max(light.r, light.g), light.b), 0.0, 1.0);
    return vec4(light, la);
}

// premultiplied src over dst
vec4 gen_over(vec4 src, vec4 dst) {
    return src + dst * (1.0 - src.a);
}

// a gradient ramp (RGBA8 sRGB-encoded stops, straight alpha) at t, decoded to linear colour. A
// macro: separate texture and sampler objects (Vulkan GLSL) cannot be passed to a function
vec4 gen_decodeRamp(vec4 c) {
    return vec4(ta_toLinear(c.rgb), c.a);
}
#define gen_ramp(lut, t) gen_decodeRamp(texture(lut, ta_lutUv(t)))
#define gen_rampLod(lut, t) gen_decodeRamp(textureLod(lut, ta_lutUv(t), 0.0))

#endif // TITLEART_GEN_COMMON_GLSL
