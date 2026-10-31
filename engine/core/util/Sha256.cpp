// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__)) && !defined(__EMSCRIPTEN__)
#define JE_SHA256_SHANI 1
#include <cpuid.h>
#include <immintrin.h>
#endif

using namespace jasperengine;

namespace {

    alignas(16) const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    inline uint32_t rotr(uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    inline uint32_t readBig(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }

    void compressPortable(uint32_t state[8], const uint8_t* data, size_t blocks) {
        uint32_t w[64];
        while (blocks--) {
            for (int i = 0; i < 16; i++)
                w[i] = readBig(data + i * 4);
            for (int i = 16; i < 64; i++) {
                const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            for (int i = 0; i < 64; i++) {
                const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                const uint32_t ch = (e & f) ^ (~e & g);
                const uint32_t t1 = h + s1 + ch + K[i] + w[i];
                const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + s0 + maj;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
            state[4] += e; state[5] += f; state[6] += g; state[7] += h;
            data += 64;
        }
    }

#ifdef JE_SHA256_SHANI
    bool cpuHasShaNi() {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
        const bool sse41 = (ecx & (1u << 19)) != 0;
        const bool ssse3 = (ecx & (1u << 9)) != 0;
        if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
        const bool sha = (ebx & (1u << 29)) != 0;
        return sse41 && ssse3 && sha;
    }

    // The Intel SHA extensions: two rounds per sha256rnds2, the message schedule by
    // sha256msg1/msg2, the state kept as ABEF/CDGH
    __attribute__((target("sha,sse4.1,ssse3")))
    void compressShaNi(uint32_t state[8], const uint8_t* data, size_t blocks) {
        const __m128i byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
        __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
        __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
        tmp = _mm_shuffle_epi32(tmp, 0xB1);
        state1 = _mm_shuffle_epi32(state1, 0x1B);
        __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
        state1 = _mm_blend_epi16(state1, tmp, 0xF0);

        while (blocks--) {
            const __m128i abefSave = state0;
            const __m128i cdghSave = state1;
            __m128i m[4];
            for (int i = 0; i < 4; i++)
                m[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i * 16)), byteSwap);
            for (int i = 0; i < 16; i++) {
                __m128i msg = _mm_add_epi32(m[i & 3], _mm_load_si128(reinterpret_cast<const __m128i*>(&K[i * 4])));
                state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
                if (i >= 3 && i < 15) {
                    const __m128i next = _mm_alignr_epi8(m[i & 3], m[(i - 1) & 3], 4);
                    m[(i + 1) & 3] = _mm_sha256msg2_epu32(_mm_add_epi32(m[(i + 1) & 3], next), m[i & 3]);
                }
                msg = _mm_shuffle_epi32(msg, 0x0E);
                state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
                if (i >= 1 && i < 13)
                    m[(i - 1) & 3] = _mm_sha256msg1_epu32(m[(i - 1) & 3], m[i & 3]);
            }
            state0 = _mm_add_epi32(state0, abefSave);
            state1 = _mm_add_epi32(state1, cdghSave);
            data += 64;
        }

        tmp = _mm_shuffle_epi32(state0, 0x1B);
        state1 = _mm_shuffle_epi32(state1, 0xB1);
        state0 = _mm_blend_epi16(tmp, state1, 0xF0);
        state1 = _mm_alignr_epi8(state1, tmp, 8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
    }
#endif

    using Compress = void (*)(uint32_t*, const uint8_t*, size_t);

    Compress compressFunction() {
#ifdef JE_SHA256_SHANI
        static const Compress chosen = cpuHasShaNi() ? &compressShaNi : &compressPortable;
        return chosen;
#else
        return &compressPortable;
#endif
    }

}

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state, initial, sizeof(state));
    buffered = 0;
    length = 0;
}

void Sha256::update(const void* data, size_t size) {
    if (size == 0) return;
    const Compress compress = compressFunction();
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    length += size;
    if (buffered > 0) {
        const size_t take = std::min(size, sizeof(buffer) - buffered);
        std::memcpy(buffer + buffered, bytes, take);
        buffered += take;
        bytes += take;
        size -= take;
        if (buffered < sizeof(buffer)) return;
        compress(state, buffer, 1);
        buffered = 0;
    }
    const size_t blocks = size / 64;
    if (blocks > 0) {
        compress(state, bytes, blocks);
        bytes += blocks * 64;
        size -= blocks * 64;
    }
    if (size > 0) {
        std::memcpy(buffer, bytes, size);
        buffered = size;
    }
}

Sha256::Digest Sha256::finish() {
    const uint64_t bits = length * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero[64] = {};
    const size_t zeros = (buffered <= 56) ? 56 - buffered : 64 - buffered + 56;
    update(zero, zeros);
    uint8_t big[8];
    for (int i = 0; i < 8; i++)
        big[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    update(big, 8);
    Digest digest;
    for (int i = 0; i < 8; i++) {
        digest[(size_t)i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
        digest[(size_t)i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest[(size_t)i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest[(size_t)i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    reset();
    return digest;
}

std::string Sha256::finishHex() {
    return toHex(finish());
}

std::string Sha256::toHex(const Digest& digest) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out;
}

bool Sha256::hardwareAccelerated() {
#ifdef JE_SHA256_SHANI
    return compressFunction() == &compressShaNi;
#else
    return false;
#endif
}

std::string jasperengine::sha256Hex(std::string_view data) {
    Sha256 hash;
    hash.update(data);
    return hash.finishHex();
}

bool jasperengine::sha256File(const std::filesystem::path& path, std::string& hex, std::string* error, uint64_t* size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    Sha256 hash;
    std::vector<char> chunk(1 << 20);
    uint64_t total = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            hash.update(chunk.data(), static_cast<size_t>(got));
            total += static_cast<uint64_t>(got);
        }
    }
    if (in.bad()) {
        if (error) *error = "read error in " + path.string();
        return false;
    }
    hex = hash.finishHex();
    if (size) *size = total;
    return true;
}
