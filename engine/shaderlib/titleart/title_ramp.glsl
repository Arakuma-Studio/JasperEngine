// The layer-space banded ramp (docs/specs/S86-title-3d.md R19): base colour x a 256-texel ramp
// (a row of the layer's ramp atlas, sRGB-encoded stops), linear, radial or reflected, over the
// layer, line, character or part box of the span table (RGBA32F, texelFetch: texel 2i the line
// box, 2i + 1 the char box), or front to back along the depth axis; bent by the layer-space
// normal: t += bend 0.25 (dot(n.xy, dir) - |n.xy|). Ported from Pachislop
// src/engine/three/materials.ts by SafEight, used with the author's permission. Reads `tl`.

#define TL_RAMP_ROWS 16.0

vec3 tlSrgbToLinear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

vec4 tlSpanBox(int span, int which) {
    return texelFetch(sampler2D(u_tlSpans, u_tlSpans_smp), ivec2(span * 2 + which, 0), 0);
}

vec3 tlRamp(int m, float row, vec3 p, vec3 n, vec2 zRange, vec4 ids, float bend) {
    vec4 m6 = tl.materials[m * TL_MATERIAL_VEC4S + 6];  // kind, angle (rad), span, depth axis
    vec4 m7 = tl.materials[m * TL_MATERIAL_VEC4S + 7];  // scale, offset
    float t;
    vec2 dir = vec2(cos(m6.y), -sin(m6.y));
    if (m6.w > 0.5) {
        t = (zRange.x - p.z) / max(zRange.x - zRange.y, 1e-4);
    } else {
        int spanKind = int(m6.z + 0.5);
        vec4 box = spanKind == 0 ? tlSpanBox(0, 0) : spanKind == 1 ? tlSpanBox(int(ids.x), 0) : tlSpanBox(int(ids.y), 1);
        vec2 c = (box.xy + box.zw) * 0.5;
        vec2 hs = max((box.zw - box.xy) * 0.5, vec2(1e-4));
        if (m6.x > 0.5 && m6.x < 1.5) {
            t = length((p.xy - c) / hs) * 0.7071068;
        } else {
            float ext = abs(hs.x * dir.x) + abs(hs.y * dir.y);
            t = dot(p.xy - c, dir) / (2.0 * ext) + 0.5;
            if (m6.x > 1.5) t = abs(t * 2.0 - 1.0);
        }
        t = (t - 0.5) / max(m7.x, 1e-3) + 0.5 - m7.y;
        t += bend * 0.25 * (dot(n.xy, dir) - length(n.xy));
    }
    vec2 uv = vec2(clamp(t, 0.0, 1.0) * (255.0 / 256.0) + 0.5 / 256.0, (row + 0.5) / TL_RAMP_ROWS);
    return tlSrgbToLinear(textureLod(sampler2D(u_tlRamps, u_tlRamps_smp), uv, 0.0).rgb);
}
