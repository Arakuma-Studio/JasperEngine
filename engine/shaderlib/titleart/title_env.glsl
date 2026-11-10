// The title environment's lookups (docs/specs/S86-title-3d.md R23, R25, R27): a linear float
// cube (or a matcap image by view normal) sampled at a size-invariant level, rotated about Y,
// repeated in azimuth and elevation when asked, floored by the blur and graded by uniforms
// (tint, Rec.709 saturation, contrast about 0.5, intensity), which are exact on mirror lookups
// because prefiltering is linear. Ported from Pachislop src/engine/three/environment.ts and
// materials.ts by SafEight, used with the author's permission. Reads `tl` (title_lit.frag).

// Roughness -> level: size invariant against a 512 face (level = 6 r + log2(face / 512)) from
// r = 1/6 up; below it the extra top levels of larger faces sharpen near-mirrors (level 0 at
// r = 0). TitleEnvironment.h's titleEnvironmentLevel is the CPU twin.
float tlEnvLevel(float r) {
    float base = tl.env.x;
    float s = r * 6.0;
    float level = s >= 1.0 ? s + base : s * (base + 1.0);
    return clamp(level, 0.0, tl.env.y);
}

vec3 tlLockNormal(vec3 n) {
    return vec3(dot(tl.lock0.xyz, n), dot(tl.lock1.xyz, n), dot(tl.lock2.xyz, n));
}

vec3 tlEnvDirection(vec3 d) {
    float c = cos(tl.frame.y), s = sin(tl.frame.y);
    d = vec3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
    if (tl.env.w > 0.5) {
        float az = atan(d.x, d.z) * tl.grade2.y;
        float el = clamp(asin(clamp(d.y, -1.0, 1.0)) * tl.grade2.z, -1.5707963, 1.5707963);
        d = vec3(sin(az) * cos(el), sin(el), cos(az) * cos(el));
    }
    return d;
}

vec3 tlGrade(vec3 c) {
    const vec3 REC709 = vec3(0.2126, 0.7152, 0.0722);
    c *= tl.grade.rgb;
    c = mix(vec3(dot(c, REC709)), c, tl.grade.a);
    float l = dot(c, REC709);
    if (l > 1e-6 && tl.grade2.x != 1.0) c *= 0.5 * pow(l / 0.5, tl.grade2.x) / l;
    return max(c, vec3(0.0)) * tl.frame.x;
}

vec3 tlEnvironment(vec3 direction, float roughness) {
    float r = max(roughness, tl.env.z);
#ifdef TL_MATCAP
    vec3 vn = normalize(vec3(dot(tl.view0.xyz, direction), dot(tl.view1.xyz, direction), 0.0) + vec3(0.0, 0.0, 1e-4));
    vec2 uv = vec2(vn.x, -vn.y) * 0.5 + 0.5;
    return tlGrade(textureLod(sampler2D(u_tlMatcap, u_tlMatcap_smp), uv, r * 6.0).rgb);
#else
    return tlGrade(textureLod(samplerCube(u_tlEnv, u_tlEnv_smp), tlEnvDirection(direction), tlEnvLevel(r)).rgb);
#endif
}
