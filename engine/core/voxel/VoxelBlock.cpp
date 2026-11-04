// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelBlock.h"

#include <cstdlib>
#include <cstring>
#include <new>

using namespace jasperengine::voxel;

namespace {

    constexpr size_t SDF_BYTES = BLOCK_SAMPLES * sizeof(int16_t);
    constexpr size_t MATERIAL_BYTES = BLOCK_SAMPLES * sizeof(uint16_t);
    // The store tags a slot's low bits, so a block's address has to leave them clear on every target
    constexpr std::align_val_t BLOCK_ALIGN{16};

    template<class T>
    bool allEqual(const T* values) {
        const T first = values[0];
        for (int i = 1; i < BLOCK_SAMPLES; i++) {
            if (values[i] != first) return false;
        }
        return true;
    }

}

size_t BlockData::payloadBytes() const {
    return (sdfValues ? SDF_BYTES : 0) + (materialValues ? MATERIAL_BYTES : 0);
}

BlockRef BlockData::createChannels(const int16_t* sdf, int16_t sdfUniform, const uint16_t* material, uint16_t materialUniform) {
    if (!sdf && !material) return BlockRef();
    const size_t header = sizeof(BlockData) + (16 - sizeof(BlockData) % 16) % 16;
    const size_t bytes = header + (sdf ? SDF_BYTES : 0) + (material ? MATERIAL_BYTES : 0);
    void* memory = ::operator new(bytes, BLOCK_ALIGN);
    auto* block = new (memory) BlockData();
    auto* cursor = static_cast<unsigned char*>(memory) + header;
    block->sdfValue = sdfUniform;
    block->materialValue = materialUniform;
    if (sdf) {
        block->sdfValues = reinterpret_cast<int16_t*>(cursor);
        std::memcpy(block->sdfValues, sdf, SDF_BYTES);
        cursor += SDF_BYTES;
    }
    if (material) {
        block->materialValues = reinterpret_cast<uint16_t*>(cursor);
        std::memcpy(block->materialValues, material, MATERIAL_BYTES);
    }
    return BlockRef::adopt(block);
}

BlockRef BlockData::create(const int16_t* sdf, const uint16_t* material) {
    const bool sdfUniform = allEqual(sdf);
    const bool materialUniform = allEqual(material);
    return createChannels(sdfUniform ? nullptr : sdf, sdf[0], materialUniform ? nullptr : material, material[0]);
}

void BlockData::release() const {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        this->~BlockData();
        ::operator delete(const_cast<BlockData*>(this), BLOCK_ALIGN);
    }
}

bool BlockContent::sameAs(const BlockContent& other) const {
    if (data.get() == other.data.get() && !data) {
        return sdfUniform == other.sdfUniform && materialUniform == other.materialUniform;
    }
    if (sdfIsUniform() != other.sdfIsUniform() || materialIsUniform() != other.materialIsUniform()) return false;
    if (sdfIsUniform()) {
        if (sdfValueIfUniform() != other.sdfValueIfUniform()) return false;
    } else if (std::memcmp(data->sdf(), other.data->sdf(), SDF_BYTES) != 0) {
        return false;
    }
    if (materialIsUniform()) return materialValueIfUniform() == other.materialValueIfUniform();
    return std::memcmp(data->material(), other.data->material(), MATERIAL_BYTES) == 0;
}

BlockContent jasperengine::voxel::makeContent(const int16_t* sdf, const uint16_t* material) {
    BlockContent content;
    content.data = BlockData::create(sdf, material);
    content.sdfUniform = sdf[0];
    content.materialUniform = material[0];
    return content;
}
