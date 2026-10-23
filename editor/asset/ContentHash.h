// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// XXH64 (the xxHash 64-bit algorithm, seed 0): the content fingerprint the asset index keeps
// to recognise a file moved outside the editor without its meta (S04 R5) and to key
// thumbnails. Written from the algorithm's published description.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace jasperengine::editor {

    class ContentHash {
    public:
        explicit ContentHash(uint64_t seed = 0);
        void update(const void* data, size_t size);
        uint64_t digest() const;

        static uint64_t of(std::string_view bytes);
        // false when the file cannot be read or is larger than maxSize
        static bool ofFile(const std::filesystem::path& path, uint64_t& hash, uint64_t maxSize = 64ull * 1024 * 1024);

    private:
        uint64_t seed;
        uint64_t total = 0;
        uint64_t v[4];
        unsigned char buffer[32];
        size_t buffered = 0;
    };

}
