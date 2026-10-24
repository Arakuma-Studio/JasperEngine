// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContentHash.h"

#include <cstring>
#include <fstream>
#include <vector>

using namespace jasperengine::editor;

namespace {

    constexpr uint64_t prime1 = 0x9E3779B185EBCA87ULL;
    constexpr uint64_t prime2 = 0xC2B2AE3D27D4EB4FULL;
    constexpr uint64_t prime3 = 0x165667B19E3779F9ULL;
    constexpr uint64_t prime4 = 0x85EBCA77C2B2AE63ULL;
    constexpr uint64_t prime5 = 0x27D4EB2F165667C5ULL;

    uint64_t rotl(uint64_t x, int r) {
        return (x << r) | (x >> (64 - r));
    }

    uint64_t read64(const unsigned char* p) {
        uint64_t value = 0;
        for (int i = 7; i >= 0; i--) {
            value = (value << 8) | p[i];
        }
        return value;
    }

    uint64_t read32(const unsigned char* p) {
        return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
               (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24);
    }

    uint64_t round(uint64_t acc, uint64_t input) {
        acc += input * prime2;
        acc = rotl(acc, 31);
        return acc * prime1;
    }

    uint64_t mergeRound(uint64_t acc, uint64_t value) {
        acc ^= round(0, value);
        return acc * prime1 + prime4;
    }

}

ContentHash::ContentHash(uint64_t seed): seed(seed) {
    v[0] = seed + prime1 + prime2;
    v[1] = seed + prime2;
    v[2] = seed;
    v[3] = seed - prime1;
}

void ContentHash::update(const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    total += size;
    if (buffered + size < 32) {
        std::memcpy(buffer + buffered, p, size);
        buffered += size;
        return;
    }
    if (buffered > 0) {
        const size_t fill = 32 - buffered;
        std::memcpy(buffer + buffered, p, fill);
        for (int i = 0; i < 4; i++) {
            v[i] = round(v[i], read64(buffer + i * 8));
        }
        p += fill;
        size -= fill;
        buffered = 0;
    }
    while (size >= 32) {
        for (int i = 0; i < 4; i++) {
            v[i] = round(v[i], read64(p + i * 8));
        }
        p += 32;
        size -= 32;
    }
    std::memcpy(buffer, p, size);
    buffered = size;
}

uint64_t ContentHash::digest() const {
    uint64_t h;
    if (total >= 32) {
        h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (int i = 0; i < 4; i++) {
            h = mergeRound(h, v[i]);
        }
    } else {
        h = seed + prime5;
    }
    h += total;

    const unsigned char* p = buffer;
    size_t remaining = buffered;
    while (remaining >= 8) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * prime1 + prime4;
        p += 8;
        remaining -= 8;
    }
    if (remaining >= 4) {
        h ^= read32(p) * prime1;
        h = rotl(h, 23) * prime2 + prime3;
        p += 4;
        remaining -= 4;
    }
    while (remaining > 0) {
        h ^= static_cast<uint64_t>(*p) * prime5;
        h = rotl(h, 11) * prime1;
        p++;
        remaining--;
    }

    h ^= h >> 33;
    h *= prime2;
    h ^= h >> 29;
    h *= prime3;
    h ^= h >> 32;
    return h;
}

uint64_t ContentHash::of(std::string_view bytes) {
    ContentHash hash;
    hash.update(bytes.data(), bytes.size());
    return hash.digest();
}

bool ContentHash::ofFile(const std::filesystem::path& path, uint64_t& result, uint64_t maxSize) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maxSize) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    ContentHash hash;
    std::vector<char> chunk(256 * 1024);
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) {
            hash.update(chunk.data(), static_cast<size_t>(got));
        }
    }
    result = hash.digest();
    return true;
}
