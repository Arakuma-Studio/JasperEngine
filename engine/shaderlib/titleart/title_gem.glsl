// Gems (docs/specs/S86-title-3d.md R21): the layer's own opaque parts seen through the gem
// (TL_GEM_BEHIND: three.js's transmission sample, as Pachislop); where nothing is behind, the environment refracted in
// and reflected back (rIn = refract(-V, N, 1 / ior_c), rOut = reflect(rIn, V)), the IOR spread
// per channel by (ior - 1) 0.025 dispersion (three lookups with TL_DISPERSION). Ported from
// Pachislop src/engine/three/materials.ts (`gemLight`) by SafEight, used with the author's
// permission. Reads `tl` through tlEnvironment.

vec3 tlGemChannel(vec3 N, vec3 V, float ior, float roughness) {
    vec3 rIn = refract(-V, N, 1.0 / max(ior, 1.0001));
    if (dot(rIn, rIn) < 1e-6) rIn = reflect(-V, N);
    return tlEnvironment(tlLockNormal(reflect(rIn, V)), roughness);
}

vec3 tlGemLight(vec3 N, vec3 V, float ior, float dispersion, float roughness) {
#ifdef TL_DISPERSION
    float spread = (ior - 1.0) * 0.025 * dispersion;
    return vec3(tlGemChannel(N, V, ior - spread, roughness).r, tlGemChannel(N, V, ior, roughness).g,
                tlGemChannel(N, V, ior + spread, roughness).b);
#else
    return tlGemChannel(N, V, ior, roughness);
#endif
}

#ifdef TL_GEM_BEHIND
// the behind copy along the view ray refracted through the gem (premultiplied colour and its
// coverage), per channel with TL_DISPERSION
vec4 tlBehindChannel(vec3 N, vec3 V, float ior, float roughness) {
    vec3 ray = normalize(refract(-V, N, 1.0 / max(ior, 1.0001))) * tl.behind.y;
    vec4 clip = tl.viewProj * vec4(v_world + ray, 1.0);
    vec2 ndc = clip.xy / max(abs(clip.w), 1e-6);
    vec2 uv = ndc * 0.5 + 0.5;
    if (tl.behind.w > 0.5) uv.y = 1.0 - uv.y;
    float lod = tl.behind.x * roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
    vec4 seen = textureLod(sampler2D(u_tlBehind, u_tlBehind_smp), uv, lod);
    // the copy without float targets holds the compressed-light encoding (behind.z 2)
    return tl.behind.z > 1.5 ? hd_decodeLight(seen) : seen;
}

vec4 tlBehind(vec3 N, vec3 V, float ior, float dispersion, float roughness) {
#ifdef TL_DISPERSION
    float spread = (ior - 1.0) * 0.025 * dispersion;
    vec4 r = tlBehindChannel(N, V, ior - spread, roughness);
    vec4 g = tlBehindChannel(N, V, ior, roughness);
    vec4 b = tlBehindChannel(N, V, ior + spread, roughness);
    return vec4(r.r, g.g, b.b, g.a);
#else
    return tlBehindChannel(N, V, ior, roughness);
#endif
}
#endif
