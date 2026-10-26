// The look of Pavel Dobryakov's WebGL-Fluid-Simulation (MIT, Copyright (c) 2017 Pavel Dobryakov;
// https://github.com/PavelDoGreat/WebGL-Fluid-Simulation): his display shading, HDR soft-knee
// bloom, sunrays and colours as functions, shared by the native fluid (shaderlib/fluid.frag,
// FluidSimulator) and the HTML UI `fluid` effect. His formulas and constants are kept; the
// dithering of the bloom is a procedural noise instead of his blue-noise PNG.
//
// The contract
//
//   Coordinates: uv in 0..1 over the target the pass writes (the bloom levels, the sunrays,
//   the display), the same frame as the dye's. `texel` is a texel of the image read (his
//   texelSize uniform).
//
//   Accessor: the includer defines, before including this file,
//
//     FLUID_LOOK_SOURCE(uv) -> vec4   the image the pass reads, with its filter (linear)
//
//   which the blur, bloom and sunrays functions read (they are defined only when it is).
//   The rest are plain functions.
//
//   The look (his render()): bloom = prefilter(dye) at bloomResolution, fluidBloomBlur down a
//   chain of halves, back up adding each level (ONE, ONE), fluidBloomBlur x intensity into the
//   bloom image; sunrays = fluidSunraysMask(dye) at the dye's size, fluidSunrays at
//   sunraysResolution, one fluidBlur pass across and one along; then the display: the dye,
//   x fluidShadingDiffuse, through fluidDisplayFinish (sunrays, gamma-encoded dithered bloom,
//   alpha = max(r, g, b)).
#ifndef FAM_FLUID_LOOK_GLSL
#define FAM_FLUID_LOOK_GLSL

// his linearToGamma: only the bloom is encoded (the dye is display values already)
vec3 fluidLinearToGamma(vec3 color){
    color = max(color, vec3(0.0));
    return max(1.055 * pow(color, vec3(0.416666667)) - 0.055, vec3(0.0));
}

// his HSVtoRGB
vec3 fluidHsvToRgb(float h, float s, float v){
    float i = floor(h * 6.0);
    float f = h * 6.0 - i;
    float p = v * (1.0 - s);
    float q = v * (1.0 - f * s);
    float t = v * (1.0 - (1.0 - f) * s);
    int sector = int(mod(i, 6.0));
    if (sector == 0) return vec3(v, t, p);
    if (sector == 1) return vec3(q, v, p);
    if (sector == 2) return vec3(p, v, t);
    if (sector == 3) return vec3(p, q, v);
    if (sector == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}

// his generateColor() for a random hue in 0..1: a pointer's colour (a random burst's is x 10)
vec3 fluidGenerateColor(float hue){
    return fluidHsvToRgb(hue, 1.0, 1.0) * 0.15;
}

// the bloom dithering: interleaved gradient noise (Jimenez 2014) of the pixel, in 0..1 (his
// one-texel-per-pixel blue-noise texture's role)
float fluidDitherNoise(vec2 fragCoord){
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

// the soft knee of applyBloom: (threshold - knee, 2 knee, 0.25 / knee),
// knee = threshold x softKnee + 0.0001
vec3 fluidBloomCurve(float threshold, float softKnee){
    float knee = threshold * softKnee + 0.0001;
    return vec3(threshold - knee, knee * 2.0, 0.25 / knee);
}

// his bloomPrefilter: what is brighter than the threshold, the knee's curve below it
vec3 fluidBloomPrefilter(vec3 c, vec3 curve, float threshold){
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - curve.x, 0.0, curve.y);
    rq = curve.z * rq * rq;
    c *= max(rq, br - threshold) / max(br, 0.0001);
    return c;
}

// his sunraysMask: the colour, alpha the light let through (dark dye lets light through)
vec4 fluidSunraysMask(vec4 c){
    float br = max(c.r, max(c.g, c.b));
    c.a = 1.0 - min(max(br * 20.0, 0.0), 0.8);
    return c;
}

// his display shading: the dye's brightness gradient as a normal (l, r, t, b: the dye one
// display texel left, right, up, down; texel: the display's texel)
float fluidShadingDiffuse(vec3 l, vec3 r, vec3 t, vec3 b, vec2 texel){
    float dx = length(r) - length(l);
    float dy = length(t) - length(b);
    vec3 n = normalize(vec3(dx, dy, length(texel)));
    vec3 light = vec3(0.0, 0.0, 1.0);
    return clamp(dot(n, light) + 0.7, 0.7, 1.0);
}

// the rest of his display: c the (shaded) dye, the bloom and the sunrays images' values, noise
// fluidDitherNoise's. Alpha = max(r, g, b) (his transparent output, premultiplied)
vec4 fluidDisplayFinish(vec3 c, vec3 bloom, float sunrays, float noise, bool useBloom, bool useSunrays){
    if (useSunrays){
        c *= sunrays;
        bloom *= sunrays;
    }
    if (useBloom){
        bloom += (noise * 2.0 - 1.0) / 255.0;
        bloom = fluidLinearToGamma(bloom);
        c += bloom;
    }
    float a = max(c.r, max(c.g, c.b));
    return vec4(c, a);
}

#ifdef FLUID_LOOK_SOURCE
// his bloomBlur: the four axis neighbours one texel of the read image away, averaged
vec4 fluidBloomBlur(vec2 uv, vec2 texel){
    vec4 sum = vec4(0.0);
    sum += FLUID_LOOK_SOURCE(uv - vec2(texel.x, 0.0));
    sum += FLUID_LOOK_SOURCE(uv + vec2(texel.x, 0.0));
    sum += FLUID_LOOK_SOURCE(uv + vec2(0.0, texel.y));
    sum += FLUID_LOOK_SOURCE(uv - vec2(0.0, texel.y));
    sum *= 0.25;
    return sum;
}

// his sunrays: the mask's alpha marched towards the centre, 16 decaying taps
float fluidSunrays(vec2 uv, float weight){
    const int ITERATIONS = 16;
    float Density = 0.3;
    float Decay = 0.95;
    float Exposure = 0.7;
    vec2 coord = uv;
    vec2 dir = uv - 0.5;
    dir *= 1.0 / float(ITERATIONS) * Density;
    float illuminationDecay = 1.0;
    float color = FLUID_LOOK_SOURCE(uv).a;
    for (int i = 0; i < ITERATIONS; i++){
        coord -= dir;
        float col = FLUID_LOOK_SOURCE(coord).a;
        color += col * illuminationDecay * weight;
        illuminationDecay *= Decay;
    }
    return color * Exposure;
}

// his blur: a 5-tap Gaussian as 3 bilinear taps along `step` (a texel across or along)
vec4 fluidBlur(vec2 uv, vec2 step){
    float offset = 1.33333333;
    vec4 sum = FLUID_LOOK_SOURCE(uv) * 0.29411764;
    sum += FLUID_LOOK_SOURCE(uv - step * offset) * 0.35294117;
    sum += FLUID_LOOK_SOURCE(uv + step * offset) * 0.35294117;
    return sum;
}
#endif

#endif // FAM_FLUID_LOOK_GLSL
