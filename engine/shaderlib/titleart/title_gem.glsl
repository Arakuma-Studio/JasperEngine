// Gems (docs/specs/S86-title-3d.md R21): where nothing is behind, the environment refracted in
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
