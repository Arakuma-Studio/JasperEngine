#version 450

// @shader mesh
// @name "Hologram"
// @category "Effects"
// @description "Sci-fi hologram: scanlines scrolling along world Y (procedural or from a texture), fresnel rim and flicker, in a tint. Additive by default (Default blend Alpha: alpha blended); no depth write, no shadows."
// @require normals position
// @state blend=additive zwrite=off shadows=off
// @state[BLEND_MODE_ALPHA] blend=alpha
// @property tint hdrcolor (0.3, 0.9, 1, 1) "Tint"
// @property fill range(0, 1) 0.2 "Fill"
// @property scanlineDensity float 14 "Scanlines per world unit"
// @property scanlineSpeed float 0.4 "Scanline speed (world units per second)"
// @property scanlineStrength range(0, 2) 0.8 "Scanline strength"
// @property scanlineSharpness range(1, 16) 3 "Scanline sharpness (procedural)"
// @property rimPower range(0.25, 8) 2 "Rim power"
// @property rimIntensity float 1.5 "Rim intensity"
// @property flickerAmount range(0, 1) 0.15 "Flicker amount"
// @property flickerSpeed float 14 "Flicker rate (changes per second)"
// @texture u_scanlineTexture white "Scanline texture (red, along V)" feature=SCANLINE_TEXTURE
// @feature SCANLINE_TEXTURE "Scanline texture"
// @keywords BLEND_MODE "Default blend" ADDITIVE ALPHA

// brightness = (fill + scanlines * strength + rim * rimIntensity) * flicker. Scanlines run
// along world Y, so they stay level whatever the object's rotation; procedural ones are
// pow(sin, sharpness) and fade to their average where they get finer than a pixel.
// The output follows the blend mode the variant draws with (BLEND_*): additive adds tint *
// brightness (the fog fades it out); the others write the tint with brightness as its
// opacity (above 1 it brightens the colour), straight alpha. BLEND_MODE picks the default
// blend (@state[BLEND_MODE_ALPHA]); a material's own blend mode wins over it. No shadows:
// a see-through glow has none.

uniform u_fs_customParams {
    vec4 tint;
    float fill;
    float scanlineDensity;
    float scanlineSpeed;
    float scanlineStrength;
    float scanlineSharpness;
    float rimPower;
    float rimIntensity;
    float flickerAmount;
    float flickerSpeed;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

#ifdef SCANLINE_TEXTURE
    uniform texture2D u_scanlineTexture;
    uniform sampler u_scanline_smp;
#endif

in vec3 v_position;
in vec3 v_normal;

out vec4 g_finalColor;

#include "includes/output.glsl"

// 0..1 from a whole number, a new value per step
float hologramHash(float step){
    return fract(sin(step * 78.233) * 43758.5453);
}

void main() {
    float time = frame.time.x;

    // in scanline units, the scroll wrapped every line so a long run keeps its precision
    float coordinate = v_position.y * customParams.scanlineDensity - fract(time * customParams.scanlineSpeed * customParams.scanlineDensity);
    #ifdef SCANLINE_TEXTURE
        float lines = texture(sampler2D(u_scanlineTexture, u_scanline_smp), vec2(0.5, coordinate)).r;
    #else
        float lines = pow(max(0.5 + 0.5 * sin(coordinate * 6.28318530718), 0.0), customParams.scanlineSharpness);
        // finer than about two pixels a line: towards the average, against moire
        // the mean of pow(0.5 + 0.5 sin, k), within half a percent
        float lineAverage = inversesqrt(3.14159265 * customParams.scanlineSharpness + 0.86);
        lines = mix(lines, lineAverage, clamp(fwidth(coordinate) * 2.0 - 0.5, 0.0, 1.0));
    #endif

    vec3 n = normalize(v_normal);
    vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    // max: |n.v| of two unit vectors can round above 1, and pow of a negative is NaN
    float rim = pow(max(1.0 - abs(dot(n, v)), 0.0), customParams.rimPower);

    // a new random dip every 1/flickerSpeed seconds (the step number kept small for the hash)
    float flicker = 1.0 - customParams.flickerAmount * hologramHash(mod(floor(time * customParams.flickerSpeed), 257.0));

    float brightness = (customParams.fill + lines * customParams.scanlineStrength + rim * customParams.rimIntensity) * flicker;

    #ifdef BLEND_ADDITIVE
        g_finalColor = finalizeColor(vec4(customParams.tint.rgb * brightness, customParams.tint.a));
    #else
        float alpha = clamp(brightness, 0.0, 1.0) * customParams.tint.a;
        g_finalColor = finalizeStraightColor(vec4(customParams.tint.rgb * max(brightness, 1.0), alpha));
    #endif
}
