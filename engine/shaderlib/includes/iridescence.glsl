// Adapted from OpenEngine (MIT), Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// (Engine/Modules/Rendering/Shaders/Includes/iridescence.glsl), after Belcour and Barla, "A
// Practical Extension to Microfacet Theory for the Modeling of Varying Iridescence", 2017.
//
// KHR_materials_iridescence: a thin film over the base. Its reflectance replaces the Fresnel
// of the base's specular, direct and image based alike (pbr_ext.glsl mixes it in by the
// iridescence factor). The spectral integral is a closed-form fit, so there is no wavelength
// loop; its constants are far below mediump's range, hence highp.

#ifndef IRIDESCENCE_GLSL
#define IRIDESCENCE_GLSL

precision highp float;

// films thinner than this (nm) show no band: the film's index fades to the air's over it
const float IRIDESCENCE_FADE_NM = 30.0;

// the CIE XYZ sensitivity of an optical path difference (nm) and phase shift (Fourier fit)
vec3 iridescenceSensitivity(float opd, float shift){
    float phase = 2.0 * M_PI * opd * 1.0e-9;
    vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3 xyz = val * sqrt(2.0 * M_PI * var) * cos(pos * phase + shift) * exp(-var * phase * phase);
    // X has a second lobe
    xyz.x += 9.7470e-14 * sqrt(2.0 * M_PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift) * exp(-4.5282e+09 * phase * phase);
    return xyz / 1.0685e-7;
}

float iridescenceF0(float ior){
    float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

// The RGB reflectance of air / film (filmIor, thickness in nm) / base (its f0), seen at
// cosTheta1 from the film's normal
vec3 iridescenceFresnel(float cosTheta1, float filmIor, float thicknessNm, vec3 baseF0){
    float eta2 = mix(1.0, filmIor, smoothstep(0.0, IRIDESCENCE_FADE_NM, thicknessNm));

    // into the film (Snell); past the critical angle the stack is a mirror
    float sinTheta2Sq = (1.0 / (eta2 * eta2)) * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2 = sqrt(max(1.0 - sinTheta2Sq, 0.0));

    // air to film: dielectric, a half-wave phase jump
    float r12 = iridescenceF0(filmIor);
    float R12 = r12 + (1.0 - r12) * pow(clamp(1.0 - cosTheta1, 0.0, 1.0), 5.0);
    float T121 = 1.0 - R12;
    float phi21 = M_PI;

    // film to base: the base's own reflectance tints the bands
    vec3 R23 = baseF0;
    float phi23 = 0.0;

    float opd = 2.0 * thicknessNm * eta2 * cosTheta2;
    float phi2 = phi21 + phi23;

    // the Airy series in the frequency domain
    vec3 R123 = R12 * R23;
    vec3 r123 = sqrt(max(R123, vec3(0.0)));
    vec3 Rs = (T121 * T121) * R23 / max(vec3(1.0) - R123, vec3(1e-4));

    vec3 C0 = vec3(R12) + Rs;
    vec3 I = C0 * iridescenceSensitivity(0.0, 0.0);

    vec3 Cm = Rs - vec3(T121);
    for (int m = 1; m <= 3; ++m){
        Cm *= r123;
        I += Cm * 2.0 * iridescenceSensitivity(float(m) * opd, float(m) * phi2);
    }

    // XYZ to linear RGB (rows sum to 1: a neutral XYZ stays neutral)
    const mat3 xyzToRgb = mat3(
         2.3706743, -0.5138850,  0.0052982,
        -0.9000405,  1.4253036, -0.0146949,
        -0.4706338,  0.0885814,  1.0093968);
    return clamp(xyzToRgb * I, 0.0, 1.0);
}

#endif
