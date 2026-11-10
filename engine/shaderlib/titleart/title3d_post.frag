#version 450
precision highp float;
precision highp int;

// The 3D title post (docs/specs/S86-title-3d.md R21, R30-R35, §6.6; ShaderType::TITLE_3D_POST,
// the pass in TP_PROGRAM), fullscreen triangles of titleart.vert addressing their target by
// gl_FragCoord in memory rows.
// - TP_PROGRAM 1 TONEMAP: the resolved main image (compressed light decoded, the MSAA shoulder
//   expanded), the AO (u_tpAux, the AO grid sampled bilinearly) multiplied into the linear
//   premultiplied colour, the glint (u_tpAux2) added as light with alpha raised by
//   (1 - a) max(g), then tone mapped on straight colour x exposure and re-premultiplied
//   (title_tonemap.glsl); display referred from here on. Debug views: the AO alone, the glint
//   alone, the ink (no glint).
// - TP_PROGRAM 2 MITCHELL_H, 3 MITCHELL_V: separable Mitchell-Netravali (B = C = 1/3) from k x
//   resolution down, 2 output px of support (4k + 1 taps), negatives clamped, alpha 0..1, on
//   premultiplied colour (Pachislop src/engine/three/resample.ts by SafEight, used with the
//   author's permission).
// - TP_PROGRAM 4 AO, 5 AO_BLUR: Scalable AO on the prepass (view normal, title-relative depth):
//   16 taps in 7 turns over the radius projected at the pixel's depth, rotated by interleaved
//   gradient noise keyed on whole-render pixels, falloff along the surface, then two 9-tap
//   depth-aware blurs (weight exp(-|dz| / (0.02 z))). Ported from Pachislop
//   src/engine/three/ao.ts by SafEight, used with the author's permission. Without float targets
//   (p0.x = 1) the prepass is packed into RGBA8 (hdr_compress.glsl hd_packNormalDepth) and the AO
//   targets are RGBA8.
// - TP_PROGRAM 6 BEHIND: the resolved opaque image as linear premultiplied light (the gem behind
//   copy's top level); 7 BOX_DOWN: a 2 x 2 box of the level above, or (p1.y = 1) the same level
//   texel for texel (the copy's mip chain, built through a second texture: a pass never samples
//   the image it draws). Without float targets the copy is SRGB8A8 in the compressed-light
//   encoding (hd_encodeLight): the box averages the decoded light, then encodes it again.
// @image_sample_type u_tpSrc unfilterable_float
// @sampler_type u_tpSrc_smp nonfiltering
// @image_sample_type u_tpAux unfilterable_float
// @sampler_type u_tpAux_smp nonfiltering
// @image_sample_type u_tpAux2 unfilterable_float
// @sampler_type u_tpAux2_smp nonfiltering

uniform texture2D u_tpSrc;
uniform sampler u_tpSrc_smp;
uniform texture2D u_tpAux;
uniform sampler u_tpAux_smp;
uniform texture2D u_tpAux2;
uniform sampler u_tpAux2_smp;

uniform u_fs_title3dPost {
    vec4 p0;    // TONEMAP: x mode, y exposure, z 1 = expand the shoulder, w the light's encoding (hd_decode);
                // AO, AO_BLUR: x 1 = the prepass is packed; BEHIND: as TONEMAP, x 1 = write the compressed
                // encoding; BOX_DOWN: x 1 = the copy holds the compressed encoding
    vec4 p1;    // MITCHELL: x k, y the source's last texel along the axis; AO: x radius (view units),
                // y px per view unit at depth 1, z intensity, w 1 = origin top left; AO_BLUR: xy the
                // axis; BOX_DOWN: x the source level, y 1 = copy the level as it is
    vec4 p2;    // TONEMAP: x 1 = AO, y 1 = glint, z debug (1 AO, 2 glint, 3 ink), w the light a glint
                // texel holds x (1, or the 8-bit glint's scale: Glint3D.h); AO, AO_BLUR: xy the
                // grid's origin in the whole render (px), z the title centre's view depth, w its radius
    vec4 p3;    // TONEMAP: the glint texture's uv = main uv x xy + zw
    mat4 projInv;   // AO: the prepass's inverse projection
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

#if TP_PROGRAM == 1
// a bilinear read of an unfilterable texture at uv (0..1, memory rows)
vec4 tpBilinearAux(vec2 uv) {
    ivec2 size = textureSize(sampler2D(u_tpAux, u_tpAux_smp), 0);
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    ivec2 last = size - 1;
    vec4 a = texelFetch(sampler2D(u_tpAux, u_tpAux_smp), clamp(i, ivec2(0), last), 0);
    vec4 b = texelFetch(sampler2D(u_tpAux, u_tpAux_smp), clamp(i + ivec2(1, 0), ivec2(0), last), 0);
    vec4 c = texelFetch(sampler2D(u_tpAux, u_tpAux_smp), clamp(i + ivec2(0, 1), ivec2(0), last), 0);
    vec4 d = texelFetch(sampler2D(u_tpAux, u_tpAux_smp), clamp(i + ivec2(1, 1), ivec2(0), last), 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec4 tpBilinearAux2(vec2 uv) {
    ivec2 size = textureSize(sampler2D(u_tpAux2, u_tpAux2_smp), 0);
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    ivec2 last = size - 1;
    vec4 a = texelFetch(sampler2D(u_tpAux2, u_tpAux2_smp), clamp(i, ivec2(0), last), 0);
    vec4 b = texelFetch(sampler2D(u_tpAux2, u_tpAux2_smp), clamp(i + ivec2(1, 0), ivec2(0), last), 0);
    vec4 c = texelFetch(sampler2D(u_tpAux2, u_tpAux2_smp), clamp(i + ivec2(0, 1), ivec2(0), last), 0);
    vec4 d = texelFetch(sampler2D(u_tpAux2, u_tpAux2_smp), clamp(i + ivec2(1, 1), ivec2(0), last), 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
#endif

#if TP_PROGRAM == 4 || TP_PROGRAM == 5
// a prepass texel: (view normal, title-relative depth), unpacked from RGBA8 when p0.x = 1
vec4 tpND(ivec2 q) {
    vec4 nd = texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), q, 0);
    return tp.p0.x > 0.5 ? hd_unpackNormalDepth(nd) : nd;
}

bool tpSurface(vec4 nd) {
    return dot(nd.xyz, nd.xyz) > 0.25;
}

float tpDepth(vec4 nd) {
    return tp.p2.z + nd.w * tp.p2.w;
}
#endif

#if TP_PROGRAM == 4
// the view-space position of the grid texel centre `q` at linear depth z (any perspective
// projection, crops included)
vec3 tpViewPos(vec2 q, vec2 size, float z) {
    vec2 uv = q / size;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, tp.p1.w > 0.5 ? 1.0 - 2.0 * uv.y : uv.y * 2.0 - 1.0);
    vec4 p = tp.projInv * vec4(ndc, -1.0, 1.0);
    vec3 d = p.xyz / p.w;
    return d * (z / -d.z);
}
#endif

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
#if TP_PROGRAM == 1
    vec4 c = texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), at, 0);
    c = hd_decode(c, tp.p0.w);
    float a = clamp(c.a, 0.0, 1.0);
    vec3 straight = a > 1e-5 ? max(c.rgb / a, vec3(0.0)) : vec3(0.0);
    if (tp.p0.z > 0.5) straight = hd_expand(straight, tp.p0.y);
    vec3 light = straight * a;
    vec2 uv = (vec2(at) + 0.5) / vec2(textureSize(sampler2D(u_tpSrc, u_tpSrc_smp), 0));
    int debugView = int(tp.p2.z + 0.5);
    if (tp.p2.x > 0.5) {
        float ao = tpBilinearAux(uv).r;
        if (debugView == 1) {
            frag_color = a > 1e-5 ? vec4(vec3(ao), 1.0) : vec4(0.0);
            return;
        }
        light *= ao;
    }
    if (tp.p2.y > 0.5) {
        vec3 g = max(tpBilinearAux2(uv * tp.p3.xy + tp.p3.zw).rgb * tp.p2.w, vec3(0.0));
        if (debugView == 2) {
            float ga = clamp(max(g.r, max(g.g, g.b)), 0.0, 1.0);
            frag_color = ga > 1e-5 ? vec4(tl_toneMap(g / ga * tp.p0.y, int(tp.p0.x + 0.5)) * ga, ga) : vec4(0.0);
            return;
        }
        if (debugView != 3) {
            light += g;
            a += (1.0 - a) * clamp(max(g.r, max(g.g, g.b)), 0.0, 1.0);
        }
    }
    if (a <= 1e-5) {
        frag_color = vec4(0.0);
        return;
    }
    vec3 col = tl_toneMap(light / a * tp.p0.y, int(tp.p0.x + 0.5));
    frag_color = vec4(col * a, a);
#elif TP_PROGRAM == 4
    vec4 nd = tpND(at);
    frag_color = vec4(1.0);
    if (!tpSurface(nd)) return;
    ivec2 isize = textureSize(sampler2D(u_tpSrc, u_tpSrc_smp), 0);
    vec2 size = vec2(isize);
    float z = tpDepth(nd);
    if (z <= 0.0) return;
    vec2 centre = vec2(at) + 0.5;
    vec3 P = tpViewPos(centre, size, z);
    vec3 N = normalize(nd.xyz);
    float radius = tp.p1.x;
    float rPx = radius * tp.p1.y / z;
    if (rPx < 1.0) return;
    // interleaved gradient noise keyed on the whole render's px (a region rotates as the whole
    // render does there)
    vec2 px = centre + tp.p2.xy;
    float rnd = 6.2831853 * fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
    float r2 = radius * radius;
    float bias = 0.02 * radius;
    float sum = 0.0;
    for (int i = 0; i < 16; i++) {
        float t = (float(i) + 0.5) / 16.0;
        float ang = t * 7.0 * 6.2831853 + rnd;
        vec2 off = vec2(cos(ang), sin(ang)) * t * rPx;
        ivec2 q = ivec2(centre + off);
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, isize))) continue;
        vec4 qnd = tpND(q);
        if (!tpSurface(qnd)) continue;
        vec3 v = tpViewPos(vec2(q) + 0.5, size, tpDepth(qnd)) - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        // falloff by the distance along the surface: a letter standing far above a band still
        // shades the band at its foot; only very tall occluders (8 radii) fade out
        float tt = max(vv - vn * vn, 0.0) / r2;
        float f = max(1.0 - tt, 0.0) * clamp(1.0 - vn / (8.0 * radius), 0.0, 1.0);
        sum += f * f * max((vn - bias) / (sqrt(vv) + 0.01 * radius), 0.0);
    }
    // the render is HDR, so occlusion reaches deep enough to read after the tone curve
    frag_color = vec4(vec3(pow(max(0.0, 1.0 - 2.0 * sum / 16.0), 2.0 * tp.p1.z)), 1.0);
#elif TP_PROGRAM == 5
    ivec2 last = textureSize(sampler2D(u_tpAux, u_tpAux_smp), 0) - 1;
    vec4 nd = tpND(at);
    if (!tpSurface(nd)) {
        frag_color = vec4(1.0);
        return;
    }
    float z = tpDepth(nd);
    ivec2 axis = ivec2(tp.p1.xy);
    float s = 0.0;
    float ws = 0.0;
    for (int i = -4; i <= 4; i++) {
        ivec2 q = clamp(at + axis * i, ivec2(0), last);
        vec4 qnd = tpND(q);
        if (!tpSurface(qnd)) continue;
        float x = float(i) / 2.0;
        float w = exp(-0.5 * x * x) * exp(-abs(tpDepth(qnd) - z) / (0.02 * max(z, 1e-4)));
        s += texelFetch(sampler2D(u_tpAux, u_tpAux_smp), q, 0).r * w;
        ws += w;
    }
    frag_color = vec4(vec3(ws > 0.0 ? s / ws : 1.0), 1.0);
#elif TP_PROGRAM == 6
    vec4 c = texelFetch(sampler2D(u_tpSrc, u_tpSrc_smp), at, 0);
    c = hd_decode(c, tp.p0.w);
    float a = clamp(c.a, 0.0, 1.0);
    vec3 straight = a > 1e-5 ? max(c.rgb / a, vec3(0.0)) : vec3(0.0);
    if (tp.p0.z > 0.5) straight = hd_expand(straight, tp.p0.y);
    frag_color = vec4(straight * a, a);
    if (tp.p0.x > 0.5) frag_color = hd_encodeLight(frag_color);
#elif TP_PROGRAM == 7
    int level = int(tp.p1.x + 0.5);
    ivec2 last = textureSize(sampler2D(u_tpAux, u_tpAux_smp), level) - 1;
    if (tp.p1.y > 0.5) {
        frag_color = texelFetch(sampler2D(u_tpAux, u_tpAux_smp), min(at, last), level);
        return;
    }
    ivec2 b = at * 2;
    float mode = tp.p0.x > 0.5 ? 1.0 : 0.0;
    vec4 sum = hd_decode(texelFetch(sampler2D(u_tpAux, u_tpAux_smp), min(b, last), level), mode);
    sum += hd_decode(texelFetch(sampler2D(u_tpAux, u_tpAux_smp), min(b + ivec2(1, 0), last), level), mode);
    sum += hd_decode(texelFetch(sampler2D(u_tpAux, u_tpAux_smp), min(b + ivec2(0, 1), last), level), mode);
    sum += hd_decode(texelFetch(sampler2D(u_tpAux, u_tpAux_smp), min(b + ivec2(1, 1), last), level), mode);
    frag_color = sum * 0.25;
    if (mode > 0.5) frag_color = hd_encodeLight(frag_color);
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
