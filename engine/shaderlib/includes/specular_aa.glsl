// Adapted from OpenEngine (MIT), Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// (Engine/Modules/Rendering/Shaders/Includes/specular_aa.glsl)
//
// Geometric specular anti-aliasing (Tokuyoshi and Kaplanyan, "Improved Geometric Specular
// Antialiasing", 2019): the screen-space variance of the shading normal widens the GGX lobe,
// so a highlight on a dense normal map or a finely curved mesh does not shimmer from pixel to
// pixel. Added in alpha^2 space and taken back to perceptual roughness as a floor: it only
// ever roughens. The built-in Lit's High quality (USE_LIT_QUALITY) calls it once per fragment,
// before the lights, in uniform control flow (derivatives).

#ifndef SPECULAR_AA_GLSL
#define SPECULAR_AA_GLSL

precision highp float;

// the variance's scale on the squared normal derivatives
const float SPECULAR_AA_VARIANCE = 0.5;
// the most it may add (silhouettes and normal map seams, where the derivative spikes)
const float SPECULAR_AA_THRESHOLD = 0.18;

// the NDF filter kernel (alpha^2 space) of a unit shading normal's derivatives
float specularAAKernel(vec3 n){
    vec3 dndx = dFdx(n);
    vec3 dndy = dFdy(n);
    float variance = SPECULAR_AA_VARIANCE * (dot(dndx, dndx) + dot(dndy, dndy));
    return min(2.0 * variance, SPECULAR_AA_THRESHOLD);
}

// perceptual roughness widened by a kernel, never below the one given
float specularAARoughness(float perceptualRoughness, float kernel){
    float alpha = perceptualRoughness * perceptualRoughness;
    float filteredAlpha2 = clamp(alpha * alpha + kernel, 0.0, 1.0);
    return max(perceptualRoughness, sqrt(sqrt(filteredAlpha2)));
}

#endif
