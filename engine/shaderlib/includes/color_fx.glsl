// Colour adjustments shared by the 2D library shaders (sprite, ui_fx, particle). All take
// and return linear RGB unless stated.

#ifndef COLOR_FX_GLSL
#define COLOR_FX_GLSL

// relative luminance of a linear colour (Rec. 709)
float colorLuminance(vec3 color){
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// rotates the colour about the grey axis (1, 1, 1) by angle radians (Rodrigues' formula):
// a hue shift that keeps grey grey; never negative
vec3 hueRotate(vec3 color, float angle){
    const vec3 k = vec3(0.57735026919); // normalize(1, 1, 1)
    float c = cos(angle);
    vec3 rotated = color * c + cross(k, color) * sin(angle) + k * (dot(k, color) * (1.0 - c));
    return max(rotated, vec3(0.0));
}

// toward the colour's own grey by amount (0 = unchanged, 1 = grey)
vec3 grayscale(vec3 color, float amount){
    return mix(color, vec3(colorLuminance(color)), amount);
}

// Hard light of a blend layer over a base, in display (sRGB-encoded) values, per channel:
// layer > 0.5 screens, below multiplies
vec3 hardLight(vec3 base, vec3 layer){
    vec3 multiplied = 2.0 * layer * base;
    vec3 screened = vec3(1.0) - 2.0 * (vec3(1.0) - layer) * (vec3(1.0) - base);
    return mix(multiplied, screened, step(vec3(0.5), layer));
}

// Alpha as light: pow(alpha * scale, power) (alpha * scale clamped to [0, 1]) times an
// HDR colour, the emission of a glow shaped by the texture's alpha
vec3 alphaGlow(float alpha, float scale, float power, vec3 hdrColor){
    return pow(clamp(alpha * scale, 0.0, 1.0), max(power, 0.001)) * hdrColor;
}

#endif
