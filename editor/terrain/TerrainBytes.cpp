// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainBytes.h"

#include "zstd.h"

#include <cstring>

std::vector<uint8_t> jasperengine::editor::packTerrainBytes(std::span<const uint8_t> raw){
    std::vector<uint8_t> packed(ZSTD_compressBound(raw.size()));
    const size_t written = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 1);
    if (ZSTD_isError(written) || written >= raw.size()){
        return std::vector<uint8_t>(raw.begin(), raw.end());
    }
    packed.resize(written);
    packed.shrink_to_fit();
    return packed;
}

bool jasperengine::editor::unpackTerrainBytes(std::span<const uint8_t> packed, std::span<uint8_t> raw){
    if (packed.size() == raw.size()){
        if (!raw.empty()) std::memcpy(raw.data(), packed.data(), raw.size());
        return true;
    }
    return ZSTD_decompress(raw.data(), raw.size(), packed.data(), packed.size()) == raw.size();
}
