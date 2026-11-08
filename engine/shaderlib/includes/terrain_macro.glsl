// T3 R9: Macro Variation's noise, two octaves of value noise over terrain-local XZ, with an integer hash
// as in includes/hex_tiling.glsl (PCG2D), so tiles far from the origin hash as well as near ones. No
// texture read. The fragment stage calls it at High and Medium, the vertex stage at Low. Loop-safe.

float terrainMacroHash(ivec2 cell){
    uvec2 h = uvec2(uint(cell.x), uint(cell.y)) * 1664525u + 1013904223u;
    h.x += h.y * 1664525u;
    h.y += h.x * 1664525u;
    h ^= (h >> 16u);
    h.x += h.y * 1664525u;
    return float(h.x >> 8u) / 16777215.0;
}

float terrainMacroValueNoise(vec2 position){
    vec2 cell = floor(position);
    vec2 f = position - cell;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(cell);
    float a = terrainMacroHash(c);
    float b = terrainMacroHash(c + ivec2(1, 0));
    float d = terrainMacroHash(c + ivec2(0, 1));
    float e = terrainMacroHash(c + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

// the tint at a terrain-local position: params x = 1 / scale, y = strength, z = slope fade; slopeUp the
// local normal's y. Between tint A and B by two values, faded out on steep ground.
vec3 terrainMacroTint(vec2 localXZ, float slopeUp, vec4 params, vec3 tintA, vec3 tintB){
    float low = terrainMacroValueNoise(localXZ * params.x);
    float high = terrainMacroValueNoise(localXZ * params.x * 2.17 + vec2(17.3, 5.1));
    vec3 tint = mix(tintA, tintB, clamp(low * 0.65 + high * 0.35, 0.0, 1.0));
    float flatness = params.z > 0.0 ? clamp((slopeUp - (1.0 - params.z)) / params.z, 0.0, 1.0) : 1.0;
    return mix(vec3(1.0), tint, params.y * flatness);
}
