#version 450
precision highp float;
precision highp int;

// The 3D title post (docs/specs/S86-title-3d.md R30, R33, R34, §6.6; ShaderType::TITLE_3D_POST,
// the pass in TP_PROGRAM), fullscreen triangles of titleart.vert addressing their target by
// gl_FragCoord in memory rows.
// - TP_PROGRAM 1 TONEMAP: the resolved main image (compressed light decoded, the MSAA shoulder
//   expanded) tone mapped on straight colour x exposure and re-premultiplied
//   (title_tonemap.glsl); display referred from here on.
// - TP_PROGRAM 2 MITCHELL_H, 3 MITCHELL_V: separable Mitchell-Netravali (B = C = 1/3) from k x
//   resolution down, 2 output px of support (4k + 1 taps), negatives clamped, alpha 0..1, on
//   premultiplied colour (Pachislop src/engine/three/resample.ts by SafEight, used with the
//   author's permission).
// @image_sample_type u_tpSrc unfilterable_float
// @sampler_type u_tpSrc_smp nonfiltering

uniform texture2D u_tpSrc;
uniform sampler u_tpSrc_smp;

uniform u_fs_title3dPost {
    vec4 p0;    // TONEMAP: x mode, y exposure, z 1 = expand the shoulder, w 1 = decode compressed light
    vec4 p1;    // MITCHELL: x k, y the source's last texel along the axis
} tp;

out vec4 frag_color;

#include "titleart/title_tonemap.glsl"
#include "titleart/hdr_compress.glsl"

float tpMitchell(float x) {
    x = abs(x);
    if (x < 1.0) return (7.0 * x * x * x - 12.0 * x * x + 16.0 / 3.0) / 6.0;
    if (x < 2.0) return (-7.0 / 3.0 * x * x * x + 12.0 * x * x - 20.0 * x + 32.0 / 3.0) / 6.0;
    return 0.0;
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
#if TP_PROGRAM == 1
    vec4 c = texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), at, 0);
    if (tp.p0.w > 0.5) c = hd_decodeLight(c);
    float a = clamp(c.a, 0.0, 1.0);
    if (a <= 1e-5) {
        frag_color = vec4(0.0);
        return;
    }
    vec3 straight = max(c.rgb / a, vec3(0.0));
    if (tp.p0.z > 0.5) straight = hd_expand(straight, tp.p0.y);
    vec3 col = tl_toneMap(straight * tp.p0.y, int(tp.p0.x + 0.5));
    frag_color = vec4(col * a, a);
#else
    float k = tp.p1.x;
    int last = int(tp.p1.y + 0.5);
    #if TP_PROGRAM == 2
        float centre = (float(at.x) + 0.5) * k;
    #else
        float centre = (float(at.y) + 0.5) * k;
    #endif
    int first = int(floor(centre - 2.0 * k));
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int i = 0; i < 17; i++) {
        int s = first + i;
        float x = (float(s) + 0.5 - centre) / k;
        if (x >= 2.0) break;
        if (s < 0 || s > last) continue;
        float w = tpMitchell(x);
        #if TP_PROGRAM == 2
            sum += texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), ivec2(s, at.y), 0) * w;
        #else
            sum += texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), ivec2(at.x, s), 0) * w;
        #endif
        weight += w;
    }
    vec4 c = sum / max(weight, 1e-6);
    frag_color = vec4(max(c.rgb, vec3(0.0)), clamp(c.a, 0.0, 1.0));
#endif
}
