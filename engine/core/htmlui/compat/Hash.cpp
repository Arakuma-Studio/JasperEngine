// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Hash.h"

#include "util/Sha256.h"

#include <cstring>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    uint32_t rotl(uint32_t value, int bits){
        return (value << bits) | (value >> (32 - bits));
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
    return jasperengine::sha256Hex(data);
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
