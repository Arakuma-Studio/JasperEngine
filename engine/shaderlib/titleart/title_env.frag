#version 450
precision highp float;
precision highp int;

// The title environment builder's passes (docs/specs/S86-title-3d.md R25, §6.5;
// ShaderType::TITLE_ENV, the pass in TE_PROGRAM), one face of one level per pass in a linear
// RGBA16F cube. Adapted from probe_filter.frag (float data instead of sRGB).
// - TE_PROGRAM 1 PROCEDURAL: a preset scene evaluated per texel analytically, 4 x 4 supersampled:
//   the sky's latitude stops, then the nearest opaque panel, disc or tube along the direction,
//   plus the additive elements in front of it (the key softbox). Ported from Pachislop
//   src/engine/three/environment.ts by SafEight, used with the author's permission; the scene
//   comes from TitleEnvironment.h's presets.
// - TE_PROGRAM 2 PREFILTER: GGX importance sampling of the source cube's mips (N = V = R), 128
//   samples, the source mip by solid angle; roughness 0 copies the top level.
// - TE_PROGRAM 3 EQUIRECT: an equirectangular HDRI to a face, 2 x 2 bilinear samples per texel,
//   oriented as Pachislop's panorama spheres (photos: a flat face mirrors the centre column),
//   plus the scene's additive elements (the softbox preset's key softbox).
// - TE_PROGRAM 4 DOWNSAMPLE: a box of the level above (the prefilter's source chain).
// v_texcoord is the destination texel's coordinate on every backend (fullscreen.vert).

in vec2 v_texcoord;
out vec4 frag_color;

#define TE_MAX_STOPS 16
#define TE_MAX_ELEMENTS 96

#if TE_PROGRAM == 2 || TE_PROGRAM == 4
uniform textureCube u_teSource;
uniform sampler u_teSource_smp;
#elif TE_PROGRAM == 3
uniform texture2D u_teEquirect;
uniform sampler u_teEquirect_smp;
#endif

uniform u_fs_titleEnv {
    vec4 params;    // x: face, y: roughness (PREFILTER), z: samples / taps, w: destination level size
    vec4 source;    // x: source base size, y: source mip count, z: source level (DOWNSAMPLE), w: stop count
    vec4 counts;    // x: element count, y: the panorama's yaw (EQUIRECT, rad)
    vec4 stops[TE_MAX_STOPS];               // (elevation, r, g, b)
    vec4 elements[TE_MAX_ELEMENTS * 3];     // per element: (kind, a.xyz), (b.xyz, size), (radiance.rgb, extra)
} te;

const float PI = 3.14159265359;

vec3 teFaceDirection(int face, vec2 st) {
    vec2 c = st * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -c.y, -c.x);
    if (face == 1) return vec3(-1.0, -c.y, c.x);
    if (face == 2) return vec3(c.x, 1.0, c.y);
    if (face == 3) return vec3(c.x, -1.0, -c.y);
    if (face == 4) return vec3(c.x, -c.y, 1.0);
    return vec3(-c.x, -c.y, -1.0);
}

#if TE_PROGRAM == 1 || TE_PROGRAM == 3
vec3 teSky(float e) {
    int n = int(te.source.w + 0.5);
    vec4 first = te.stops[0];
    if (e <= first.x) return first.yzw;
    for (int k = 0; k < TE_MAX_STOPS - 1; k++) {
        if (k + 1 >= n) break;
        vec4 a = te.stops[k], b = te.stops[k + 1];
        if (e <= b.x) return mix(a.yzw, b.yzw, (e - a.x) / max(b.x - a.x, 1e-6));
    }
    return te.stops[max(n - 1, 0)].yzw;
}

float teSmooth(float x) { return x * x * (3.0 - 2.0 * x); }

// a plane element facing the origin (three's lookAt: +z toward the target, x = up x z)
bool tePlane(vec3 d, vec3 c, out float t, out vec2 uv) {
    vec3 z = normalize(-c);
    float den = dot(d, -z);
    t = 0.0;
    uv = vec2(0.0);
    if (den <= 1e-6) return false;
    t = dot(c, -z) / den;
    if (t <= 0.0) return false;
    vec3 up = abs(z.y) > 0.9999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 x = normalize(cross(up, z));
    vec3 y = cross(z, x);
    vec3 h = d * t - c;
    uv = vec2(dot(h, x), dot(h, y));
    return true;
}

vec3 teScene(vec3 d) {
    float nearest = 1e9;
    vec3 color = teSky(d.y);
    vec3 added = vec3(0.0);
    float addedAt = 1e9;
    int n = int(te.counts.x + 0.5);
    for (int i = 0; i < TE_MAX_ELEMENTS; i++) {
        if (i >= n) break;
        vec4 e0 = te.elements[i * 3], e1 = te.elements[i * 3 + 1], e2 = te.elements[i * 3 + 2];
        int kind = int(e0.x + 0.5);
        float t;
        vec2 uv;
        if (kind == 0 || kind == 3) {
            // 0 panel (w, h in e1.xy), 3 additive soft panel (the key softbox, edge in e2.w)
            if (!tePlane(d, e0.yzw, t, uv) || abs(uv.x) > e1.x * 0.5 || abs(uv.y) > e1.y * 0.5) continue;
            if (kind == 3) {
                float ex = (0.5 - abs(uv.x) / e1.x) * 2.0, ey = (0.5 - abs(uv.y) / e1.y) * 2.0;
                float f = teSmooth(min(1.0, ex / e2.w)) * teSmooth(min(1.0, ey / e2.w));
                added += e2.rgb * f;
                addedAt = min(addedAt, t);
                continue;
            }
        } else if (kind == 1) {
            // disc (radius e1.w)
            if (!tePlane(d, e0.yzw, t, uv) || length(uv) > e1.w) continue;
        } else {
            // tube from a (e0.yzw) to b (e1.xyz), radius e1.w: the ray from the origin against the cylinder
            vec3 a = e0.yzw, axis = e1.xyz - a;
            float len = length(axis);
            vec3 w = axis / max(len, 1e-6);
            vec3 dp = d - w * dot(d, w);
            vec3 ap = -a - w * dot(-a, w);
            float A = dot(dp, dp), B = 2.0 * dot(dp, ap), C = dot(ap, ap) - e1.w * e1.w;
            float disc = B * B - 4.0 * A * C;
            if (A < 1e-9 || disc < 0.0) continue;
            t = (-B - sqrt(disc)) / (2.0 * A);
            if (t <= 0.0) continue;
            float s = dot(d * t - a, w);
            if (s < 0.0 || s > len) continue;
        }
        if (t < nearest) {
            nearest = t;
            color = e2.rgb;
        }
    }
    // additive elements show only in front of the nearest opaque hit
    if (addedAt < nearest) color += added;
    return color;
}

// an HDRI's additive elements (the softbox preset's key softbox)
vec3 teAdditive(vec3 d) {
    vec3 added = vec3(0.0);
    int n = int(te.counts.x + 0.5);
    for (int i = 0; i < TE_MAX_ELEMENTS; i++) {
        if (i >= n) break;
        vec4 e0 = te.elements[i * 3], e1 = te.elements[i * 3 + 1], e2 = te.elements[i * 3 + 2];
        if (int(e0.x + 0.5) != 3) continue;
        float t;
        vec2 uv;
        if (!tePlane(d, e0.yzw, t, uv) || abs(uv.x) > e1.x * 0.5 || abs(uv.y) > e1.y * 0.5) continue;
        float ex = (0.5 - abs(uv.x) / e1.x) * 2.0, ey = (0.5 - abs(uv.y) / e1.y) * 2.0;
        added += e2.rgb * teSmooth(min(1.0, ex / e2.w)) * teSmooth(min(1.0, ey / e2.w));
    }
    return added;
}
#endif

#if TE_PROGRAM == 2
float teRadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec3 teFilterGGX(vec3 n, float roughness) {
    const int MAX_SAMPLES = 128;
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    int count = int(te.params.z + 0.5);
    float sourceSize = te.source.x;
    float texelSolidAngle = 4.0 * PI / (6.0 * sourceSize * sourceSize);
    float maxLod = te.source.y - 1.0;
    vec3 color = vec3(0.0);
    float weight = 0.0;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (i >= count) break;
        vec2 xi = vec2(float(i) / float(count), teRadicalInverse(uint(i)));
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        vec3 h = normalize(tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + n * cosTheta);
        vec3 l = 2.0 * dot(n, h) * h - n;
        float ndotl = dot(n, l);
        if (ndotl > 0.0) {
            float dd = cosTheta * cosTheta * (a2 - 1.0) + 1.0;
            float pdf = a2 / (PI * dd * dd) * 0.25 + 0.0001;
            float sampleSolidAngle = 1.0 / (float(count) * pdf);
            float lod = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, maxLod);
            color += textureLod(samplerCube(u_teSource, u_teSource_smp), l, lod).rgb * ndotl;
            weight += ndotl;
        }
    }
    return color / max(weight, 0.0001);
}
#endif

void main() {
    int face = int(te.params.x + 0.5);
    float texel = 1.0 / te.params.w;
    vec3 color = vec3(0.0);
#if TE_PROGRAM == 1
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec2 offset = (vec2(float(i), float(j)) + 0.5) / 4.0 - 0.5;
            color += teScene(normalize(teFaceDirection(face, v_texcoord + offset * texel)));
        }
    }
    color /= 16.0;
#elif TE_PROGRAM == 2
    vec3 n = normalize(teFaceDirection(face, v_texcoord));
    if (te.params.y <= 0.0) {
        color = textureLod(samplerCube(u_teSource, u_teSource_smp), n, max(0.0, log2(te.source.x / te.params.w))).rgb;
    } else {
        color = teFilterGGX(n, te.params.y);
    }
#elif TE_PROGRAM == 3
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            vec2 offset = (vec2(float(i), float(j)) + 0.5) / 2.0 - 0.5;
            vec3 d = normalize(teFaceDirection(face, v_texcoord + offset * texel));
            // Pachislop's inside-out panorama sphere turned by `yaw` about Y (softbox 0: the
            // image's centre column at +x; photos pi/2: at +z), sky up
            float c = cos(te.counts.y), s = sin(te.counts.y);
            vec3 l = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
            vec2 uv = vec2(atan(l.z, -l.x) / (2.0 * PI), acos(clamp(l.y, -1.0, 1.0)) / PI);
            color += textureLod(sampler2D(u_teEquirect, u_teEquirect_smp), vec2(fract(uv.x), uv.y), 0.0).rgb + teAdditive(d);
        }
    }
    color /= 4.0;
#else
    int taps = int(te.params.z + 0.5);
    for (int j = 0; j < 4; j++) {
        if (j >= taps) break;
        for (int i = 0; i < 4; i++) {
            if (i >= taps) break;
            vec2 offset = (vec2(float(i), float(j)) + 0.5) / float(taps) - 0.5;
            color += textureLod(samplerCube(u_teSource, u_teSource_smp), teFaceDirection(face, v_texcoord + offset * texel), te.source.z).rgb;
        }
    }
    color /= float(taps * taps);
#endif
    frag_color = vec4(color, 1.0);
}
