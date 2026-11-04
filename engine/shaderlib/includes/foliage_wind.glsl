// Foliage's wind and fade (T5 F6, F8, R11, R19), the one function of the colour, depth and G-buffer
// programs, so shadows sway with the plant. After library/foliage.glsl: the same sway, gust and
// flutter, with the plant's bend and phase from its instance. Everything is in terrain-local space,
// so an instance's position needs no model matrix. Include after foliage_instance.glsl.

#ifndef FOLIAGE_WIND_GLSL
#define FOLIAGE_WIND_GLSL

#if defined(USE_FOLIAGE_WIND) || defined(USE_FOLIAGE_DITHER) || defined(FOLIAGE_IMPOSTOR)

uniform u_vs_foliage {
    vec4 fadeRange;     // x, y: 0 before x, 1 from y on. z, w: 1 before z, 0 from w on
    vec4 eye;           // the view's camera, terrain-local; w seconds, what frame.time.x holds
    vec4 wind;          // xyz downwind, terrain-local, length the Terrain's Bending; w cycles per second
    vec4 params;        // x waves per unit, y gust, z flutter, w 1: vertex colour red masks the flutter
    vec4 heightRange;   // x start, y range, in the mesh's own units; z 1: two impostor views blend; w impostor views
    vec4 impostor;      // xyz the impostor's bounds centre; w width
    vec4 impostorSize;  // x height
} foliageParams;

#endif

#ifdef USE_FOLIAGE_WIND

const float FOLIAGE_TWO_PI = 6.28318530718;

// position and origin in terrain-local space; height: above the base, in the mesh's own units;
// red: the vertex colour's, when it masks the flutter
vec3 foliageWindSway(vec3 position, vec3 origin, float height, float red){
    float bend = i_packed3.z;
    float amount = length(foliageParams.wind.xyz);
    if (amount <= 0.0 || bend <= 0.0) return position;
    vec3 direction = foliageParams.wind.xyz / amount;
    float time = foliageParams.eye.w;

    // the whole plant sways as one, its phase from where it stands along the wind plus its own,
    // so a field ripples with waves travelling downwind; phases in cycles, wrapped before the sine
    float along = dot(origin, direction) * foliageParams.params.x + i_packed3.y;
    float sway = sin(fract(time * foliageParams.wind.w - along) * FOLIAGE_TWO_PI);
    float gust = 0.5 + 0.5 * sin(fract(time * foliageParams.wind.w * 0.21 - along * 0.37) * FOLIAGE_TWO_PI);
    gust *= gust;

    // grows with height squared: a stem bends more towards its tip
    float reach = clamp((height - foliageParams.heightRange.x) / max(foliageParams.heightRange.y, 1e-4), 0.0, 1.0);
    float whole = reach * reach * (sway + foliageParams.params.y * gust * 2.0);

    // leaves flutter on their own, from where the vertex is (not in an impostor: it has no leaves)
    float flutterMask = foliageParams.params.w > 0.5 ? red : reach;
    float flutter = sin(fract(time * foliageParams.wind.w * 3.1 + dot(position, vec3(0.73, 0.41, 0.59)) * 3.0) * FOLIAGE_TWO_PI);

    return position + direction * amount * bend * (whole + foliageParams.params.z * flutterMask * flutter);
}

#endif

#ifdef USE_FOLIAGE_DITHER

// F6: from instance distance, on XZ as the band's. 1 shown, 0 gone.
float foliageFadeFactor(vec3 origin){
    float distance = length(foliageParams.eye.xz - origin.xz);
    float fadeIn = foliageParams.fadeRange.y > foliageParams.fadeRange.x
        ? clamp((distance - foliageParams.fadeRange.x) / (foliageParams.fadeRange.y - foliageParams.fadeRange.x), 0.0, 1.0) : 1.0;
    float fadeOut = foliageParams.fadeRange.w > foliageParams.fadeRange.z
        ? clamp((foliageParams.fadeRange.w - distance) / (foliageParams.fadeRange.w - foliageParams.fadeRange.z), 0.0, 1.0) : 1.0;
    return fadeIn * fadeOut;
}

#endif

#endif
