// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// three.js's thin-film iridescence (evalIridescence, evalSensitivity, Schlick_to_F0 of
// lights_physical_pars_fragment and bsdfs; MIT, Copyright 2010-2025 three.js authors), which
// Pachislop's MeshPhysicalMaterial uses (src/engine/three/materials.ts, ported by SafEight, used
// with the author's permission). Unlike includes/iridescence.glsl (the engine's PBR), the base
// is seen through the film: its index relative to the film's sets the second interface and a
// half-wave phase jump where the base is optically thinner than the film (a dielectric base
// under Pachislop's 1.6 film), which shifts every band's hue.

#ifndef TITLE_IRIDESCENCE_GLSL
#define TITLE_IRIDESCENCE_GLSL

float tlIorToFresnel0(float transmittedIor, float incidentIor) {
    float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

vec3 tlIorToFresnel0(vec3 transmittedIor, float incidentIor) {
    vec3 r = (transmittedIor - vec3(incidentIor)) / (transmittedIor + vec3(incidentIor));
    return r * r;
}

vec3 tlFresnel0ToIor(vec3 fresnel0) {
    vec3 s = sqrt(fresnel0);
    return (vec3(1.0) + s) / (vec3(1.0) - s);
}

// the linear Rec. 709 response of an optical path difference (nm) and phase shift (Fourier fit
// of the CIE XYZ sensitivity)
vec3 tlIridescenceSensitivity(float opd, vec3 shift) {
    float phase = 2.0 * M_PI * opd * 1.0e-9;
    vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3 xyz = val * sqrt(2.0 * M_PI * var) * cos(pos * phase + shift) * exp(-phase * phase * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * M_PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) * exp(-4.5282e+09 * phase * phase);
    xyz /= 1.0685e-7;
    const mat3 XYZ_TO_REC709 = mat3(3.2404542, -0.9692660, 0.0556434,
                                    -1.5371385, 1.8760108, -0.2040259,
                                    -0.4985314, 0.0415560, 1.0572252);
    return XYZ_TO_REC709 * xyz;
}

// the film's reflectance (air outside) seen at cosTheta1, over a base of reflectance baseF0
vec3 tlIridescenceFresnel(float cosTheta1, float filmIor, float thicknessNm, vec3 baseF0) {
    const float outsideIor = 1.0;
    float eta2 = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thicknessNm));
    float sinTheta2Sq = (outsideIor / eta2) * (outsideIor / eta2) * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) return vec3(1.0);
    float cosTheta2 = sqrt(cosTheta2Sq);

    float R0 = tlIorToFresnel0(eta2, outsideIor);
    float R12 = R0 + (1.0 - R0) * pow(clamp(1.0 - cosTheta1, 0.0, 1.0), 5.0);
    float T121 = 1.0 - R12;
    float phi12 = eta2 < outsideIor ? M_PI : 0.0;
    float phi21 = M_PI - phi12;

    vec3 baseIor = tlFresnel0ToIor(clamp(baseF0, 0.0, 0.9999));
    vec3 R1 = tlIorToFresnel0(baseIor, eta2);
    vec3 R23 = R1 + (vec3(1.0) - R1) * pow(clamp(1.0 - cosTheta2, 0.0, 1.0), 5.0);
    vec3 phi23 = vec3(baseIor.x < eta2 ? M_PI : 0.0, baseIor.y < eta2 ? M_PI : 0.0, baseIor.z < eta2 ? M_PI : 0.0);

    float opd = 2.0 * eta2 * thicknessNm * cosTheta2;
    vec3 phi = vec3(phi21) + phi23;

    vec3 R123 = clamp(R12 * R23, 1e-5, 0.9999);
    vec3 r123 = sqrt(R123);
    vec3 Rs = T121 * T121 * R23 / (vec3(1.0) - R123);
    vec3 I = vec3(R12) + Rs;
    vec3 Cm = Rs - vec3(T121);
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123;
        I += Cm * 2.0 * tlIridescenceSensitivity(float(m) * opd, float(m) * phi);
    }
    return max(I, vec3(0.0));
}

// the F0 that Schlick's curve turns into `f` at cosTheta (three.js's Schlick_to_F0): the
// iridescent F0 the image-based and direct lobes take
vec3 tlSchlickToF0(vec3 f, float f90, float cosTheta) {
    float x = clamp(1.0 - cosTheta, 0.0, 1.0);
    float x5 = clamp(x * x * x * x * x, 0.0, 0.9999);
    return (f - vec3(f90) * x5) / (1.0 - x5);
}

#endif
