// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Guid::derive, fnvHash and splitMix64 are adapted from OpenEngine
// (Engine/Modules/AssetCore/Source/GUID.cpp, GUID::Derive, FnvHash, SplitMix64):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#include "Guid.h"

using namespace jasperengine;

namespace {

    // FNV-1a over a byte sequence, with an explicit 64-bit accumulator so 32-bit builds
    // derive the same bits as the 64-bit editor
    uint64_t fnvHash(const uint8_t* data, size_t length, uint64_t seed = 14695981039346656037ULL) {
        uint64_t h = seed;
        for (size_t i = 0; i < length; ++i) {
            h ^= static_cast<uint64_t>(data[i]);
            h *= 1099511628211ULL; // FNV prime
        }
        return h;
    }

    // SplitMix64 finalizer: avalanches the lower word into a well-distributed upper word
    uint64_t splitMix64(uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string_view trimmed(std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
        return text;
    }

}

bool Guid::isValidText(std::string_view text) {
    text = trimmed(text);
    if (text.size() != 32) {
        return false;
    }
    for (char c : text) {
        if (hexValue(c) < 0) {
            return false;
        }
    }
    return true;
}

Guid Guid::parse(std::string_view hex32) {
    hex32 = trimmed(hex32);
    if (!isValidText(hex32)) {
        return Guid();
    }
    Guid guid;
    for (size_t i = 0; i < 16; i++) {
        guid.hi = (guid.hi << 4) | static_cast<uint64_t>(hexValue(hex32[i]));
    }
    for (size_t i = 16; i < 32; i++) {
        guid.lo = (guid.lo << 4) | static_cast<uint64_t>(hexValue(hex32[i]));
    }
    return guid;
}

std::string Guid::str() const {
    if (isNull()) {
        return std::string();
    }
    static const char digits[] = "0123456789abcdef";
    std::string text(32, '0');
    for (int i = 0; i < 16; i++) {
        text[15 - i] = digits[(hi >> (i * 4)) & 0xF];
        text[31 - i] = digits[(lo >> (i * 4)) & 0xF];
    }
    return text;
}

void Guid::toBytes(uint8_t out[16]) const {
    for (int i = 0; i < 8; i++) {
        out[i] = static_cast<uint8_t>((hi >> ((7 - i) * 8)) & 0xFF);
        out[8 + i] = static_cast<uint8_t>((lo >> ((7 - i) * 8)) & 0xFF);
    }
}

Guid Guid::fromBytes(const uint8_t bytes[16]) {
    Guid guid;
    for (int i = 0; i < 8; i++) {
        guid.hi = (guid.hi << 8) | bytes[i];
        guid.lo = (guid.lo << 8) | bytes[8 + i];
    }
    return guid;
}

Guid Guid::derive(const Guid& parent, std::string_view key) {
    // Lower 64 bits: FNV-1a over the parent's 16 bytes, '|' and the key's bytes
    uint8_t parentBytes[16];
    parent.toBytes(parentBytes);
    uint64_t h1 = fnvHash(parentBytes, 16);
    const uint8_t separator = '|';
    h1 = fnvHash(&separator, 1, h1);
    h1 = fnvHash(reinterpret_cast<const uint8_t*>(key.data()), key.size(), h1);

    // Upper 64 bits: the lower hash avalanched
    const uint64_t h2 = splitMix64(h1);

    uint8_t data[16];
    for (int i = 0; i < 8; ++i) {
        data[i] = static_cast<uint8_t>((h1 >> (i * 8)) & 0xFF);
        data[8 + i] = static_cast<uint8_t>((h2 >> (i * 8)) & 0xFF);
    }
    // RFC 4122-like version and variant bits
    data[6] = static_cast<uint8_t>((data[6] & 0x0F) | 0x40);
    data[8] = static_cast<uint8_t>((data[8] & 0x3F) | 0x80);
    return fromBytes(data);
}
