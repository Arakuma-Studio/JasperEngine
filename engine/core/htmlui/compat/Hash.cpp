// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Hash.h"

#include <cstring>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    uint32_t rotl(uint32_t value, int bits){
        return (value << bits) | (value >> (32 - bits));
    }

    uint32_t rotr(uint32_t value, int bits){
        return (value >> bits) | (value << (32 - bits));
    }

    // the message padded to whole 64-byte blocks with its bit length (big endian)
    std::vector<unsigned char> pad(std::string_view data){
        std::vector<unsigned char> message(data.begin(), data.end());
        const uint64_t bits = (uint64_t)data.size() * 8;
        message.push_back(0x80);
        while (message.size() % 64 != 56)
            message.push_back(0);
        for (int i = 7; i >= 0; i--)
            message.push_back((unsigned char)(bits >> (i * 8)));
        return message;
    }

    uint32_t readBig(const unsigned char* p){
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }

    std::string wordsToHex(const uint32_t* words, int count){
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve((size_t)count * 8);
        for (int i = 0; i < count; i++){
            for (int shift = 28; shift >= 0; shift -= 4)
                out += hex[(words[i] >> shift) & 0xF];
        }
        return out;
    }

}

std::string jasperengine::htmlui::compat::sha1Hex(std::string_view data){
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::vector<unsigned char> message = pad(data);
    uint32_t w[80];
    for (size_t block = 0; block < message.size(); block += 64){
        for (int i = 0; i < 16; i++)
            w[i] = readBig(&message[block + (size_t)i * 4]);
        for (int i = 16; i < 80; i++)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++){
            uint32_t f, k;
            if (i < 20){ f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40){ f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    return wordsToHex(h, 5);
}

std::string jasperengine::htmlui::compat::sha256Hex(std::string_view data){
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::vector<unsigned char> message = pad(data);
    uint32_t w[64];
    for (size_t block = 0; block < message.size(); block += 64){
        for (int i = 0; i < 16; i++)
            w[i] = readBig(&message[block + (size_t)i * 4]);
        for (int i = 16; i < 64; i++){
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++){
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return wordsToHex(h, 8);
}

uint64_t jasperengine::htmlui::compat::fnv1a64(std::string_view data, uint64_t seed){
    uint64_t hash = seed;
    for (unsigned char c : data){
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::string jasperengine::htmlui::compat::toHex(uint64_t value, int digits){
    static const char* hex = "0123456789abcdef";
    std::string out((size_t)digits, '0');
    for (int i = digits - 1; i >= 0; i--){
        out[(size_t)i] = hex[value & 0xF];
        value >>= 4;
    }
    return out;
}
