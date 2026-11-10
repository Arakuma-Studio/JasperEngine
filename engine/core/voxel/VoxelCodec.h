// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_CODEC_H
#define VOXEL_CODEC_H

// V9's DRLE16: delta from the previous sample, zero-run tokens. A player has no zstd compressor
// (R79), so a runtime delta uses this; it is also a fine fallback where zstd does not pay.
//
// Stream of tokens, little-endian: a byte b.
//   b < 0x80: a run of (b + 1) zero deltas.
//   b >= 0x80: (b - 0x7F) literal deltas follow, each 2 bytes.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::voxel {

    JASPERENGINE_API void drle16Encode(const uint16_t* values, size_t count, std::vector<uint8_t>& out);
    // Bounds-checked: false unless the bytes produce exactly `count` values and end there
    JASPERENGINE_API bool drle16Decode(const uint8_t* bytes, size_t size, uint16_t* values, size_t count);

}

#endif
