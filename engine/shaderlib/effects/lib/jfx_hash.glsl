// HTML UI effects: hashes. Ported from the shaders.com kit noise.ts (MIT, Copyright (c) 2026
// Shader Effects Inc.; see effects/LICENSE.shaders-com.txt): the bit-cast "Fi" hashes
// (hash11 ... hash33) and MaterialX's lookup3 integer hash. The constants are bit-exact; the
// float hashes need highp (SPIRV-Cross declares it for GLSL ES).
#ifndef JFX_HASH_GLSL
#define JFX_HASH_GLSL

// float(~0u) as the kit writes it (4294967295.0 rounds to 4294967296.0 in f32)
#define JFX_U32_MAX 4294967295.0

float jfx_hash11(float p){
    uint u = floatBitsToUint(p * 3141592653.0);
    return float(u * u * 3141592653u) / JFX_U32_MAX;
}

float jfx_hash12(vec2 p){
    uvec2 u = floatBitsToUint(p * vec2(141421356.0, 2718281828.0));
    return float((u.x ^ u.y) * 3141592653u) / JFX_U32_MAX;
}

vec2 jfx_hash22(vec2 p){
    uvec2 u = floatBitsToUint(p * vec2(141421356.0, 2718281828.0));
    uint h = u.x ^ u.y;
    return vec2(float(h * 3141592653u), float(h * 1618033988u)) / JFX_U32_MAX;
}

vec3 jfx_hash32(vec2 p){
    uvec2 u = floatBitsToUint(p * vec2(141421356.0, 2718281828.0));
    uint h = u.x ^ u.y;
    return vec3(float(h * 1732050807u), float(h * 2645751311u), float(h * 3316624790u)) / JFX_U32_MAX;
}

float jfx_hash13(vec3 p){
    uvec3 u = floatBitsToUint(p * vec3(141421356.0, 2718281828.0, 1618033988.0));
    return float((u.x ^ u.y ^ u.z) * 3141592653u) / JFX_U32_MAX;
}

vec3 jfx_hash33(vec3 p){
    uvec3 u = floatBitsToUint(p * vec3(141421356.0, 2718281828.0, 1618033988.0));
    uint h = u.x ^ u.y ^ u.z;
    return vec3(float(h * 1732050807u), float(h * 2645751311u), float(h * 3316624790u)) / JFX_U32_MAX;
}

// ---- MaterialX (Bob Jenkins lookup3): noise.ts mxRotl32, mxBjfinal, mxHashInt1/2

uint jfx_mx_rotl32(uint x, int k){
    return (x << uint(k)) | (x >> uint(32 - k));
}

uint jfx_mx_bjfinal(uint a, uint b, uint c){
    c = c ^ b; c = c - jfx_mx_rotl32(b, 14);
    a = a ^ c; a = a - jfx_mx_rotl32(c, 11);
    b = b ^ a; b = b - jfx_mx_rotl32(a, 25);
    c = c ^ b; c = c - jfx_mx_rotl32(b, 16);
    a = a ^ c; a = a - jfx_mx_rotl32(c, 4);
    b = b ^ a; b = b - jfx_mx_rotl32(a, 14);
    c = c ^ b; c = c - jfx_mx_rotl32(b, 24);
    return c;
}

uint jfx_mx_hash(int x, int y){
    uint seed = 0xdeadbeefu + (2u << 2u) + 13u;
    return jfx_mx_bjfinal(seed + uint(x), seed + uint(y), seed);
}

uint jfx_mx_hash(int x, int y, int z){
    uint seed = 0xdeadbeefu + (3u << 2u) + 13u;
    return jfx_mx_bjfinal(seed + uint(x), seed + uint(y), seed + uint(z));
}

float jfx_mx_bits_to_01(uint bits){
    return float(bits) / JFX_U32_MAX;
}

#endif // JFX_HASH_GLSL
