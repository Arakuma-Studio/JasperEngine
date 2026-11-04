// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::editor{

    // What an undo step keeps of map bytes (T1 R24, R28, DC7): zstd level 1 when that saves bytes, the
    // bytes themselves otherwise. Which one it is shows in the size, so nothing else is stored.
    std::vector<uint8_t> packTerrainBytes(std::span<const uint8_t> raw);

    // False when packed is not what packTerrainBytes made of raw.size() bytes
    bool unpackTerrainBytes(std::span<const uint8_t> packed, std::span<uint8_t> raw);

}
