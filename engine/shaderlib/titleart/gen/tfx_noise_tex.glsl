// The mobile tier's noise (docs/specs/S83-title-effects-generators.md §6.7.11, §6.7.12, D12):
// one fetch of the tiling noise texture (generators/TitleGenMobile.h mobileNoiseData) instead
// of several of Pachislop's value noises (tfx_noise.glsl). R and G are two independent fbm-like
// fields (mean 0.5) repeating every 255 texels, and texel 255 holds texel 0's value, so the
// lookup below is seamless with LINEAR filtering and clamped edges. One lattice unit of
// Pachislop's noise is 255 / 16 texels: the coarsest octave's cell. The texture is in TA_TEX1.

#ifndef TITLEART_GEN_TFX_NOISE_TEX_GLSL
#define TITLEART_GEN_TFX_NOISE_TEX_GLSL

// both fields at p (lattice units)
vec2 gen_noiseTex(vec2 p) {
    vec2 t = fract(p / 16.0) * 255.0 + 0.5;
    return texture(TA_TEX1, t / 256.0).rg;
}

#endif // TITLEART_GEN_TFX_NOISE_TEX_GLSL
