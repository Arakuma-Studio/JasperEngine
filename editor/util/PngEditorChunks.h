// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's own PNG text chunks (iTXt / tEXt / zTXt whose keyword starts with "jasper.": the
// sprite normals recipe and paint layer, the texture synthesis recipe) dropped from a PNG, for the
// exporter: a game never reads them. Lossless: every other chunk is kept byte for byte (its CRC
// included), and the image data is not decoded.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace jasperengine::editor::pngchunks {

    // Whether the PNG in `bytes` carries an editor chunk; `out` is then the PNG without them. False
    // (out untouched) for a PNG without one, or bytes that are not a well-formed PNG chunk stream
    // (those are copied as they are)
    inline bool stripEditorChunks(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out) {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) return false;
        struct Span {
            size_t begin, end;
        };
        std::vector<Span> kept;
        bool dropped = false, ended = false;
        size_t pos = 8;
        while (pos < bytes.size()) {
            if (bytes.size() - pos < 12) return false;
            const uint32_t length = (uint32_t(bytes[pos]) << 24) | (uint32_t(bytes[pos + 1]) << 16) | (uint32_t(bytes[pos + 2]) << 8) | uint32_t(bytes[pos + 3]);
            if (length > bytes.size() - pos - 12) return false;
            const uint8_t* type = &bytes[pos + 4];
            const size_t end = pos + 12 + length;
            const bool text = std::memcmp(type, "iTXt", 4) == 0 || std::memcmp(type, "tEXt", 4) == 0 || std::memcmp(type, "zTXt", 4) == 0;
            if (text && length >= 7 && std::memcmp(&bytes[pos + 8], "jasper.", 7) == 0) {
                dropped = true;
            } else {
                kept.push_back({pos, end});
            }
            ended = std::memcmp(type, "IEND", 4) == 0;
            pos = end;
            if (ended) break;
        }
        if (!ended || !dropped) return false;
        std::vector<uint8_t> result(bytes.begin(), bytes.begin() + 8);
        for (const Span& span : kept) result.insert(result.end(), bytes.begin() + static_cast<std::ptrdiff_t>(span.begin), bytes.begin() + static_cast<std::ptrdiff_t>(span.end));
        // anything after IEND (some writers append bytes) stays as it was
        result.insert(result.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
        out = std::move(result);
        return true;
    }

}
