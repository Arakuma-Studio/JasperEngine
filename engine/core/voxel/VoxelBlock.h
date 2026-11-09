// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_BLOCK_H
#define VOXEL_BLOCK_H

// One block's samples (R2-R6). A block is immutable once published, and reference counted, so an
// edit writes a new one and a job holding a reference reads a consistent version without a lock. A
// channel whose 4096 values are equal stores the value alone; a block with both channels uniform
// never allocates and lives inline in the store's 8-byte slot.

#include "Export.h"
#include "VoxelTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace jasperengine::voxel {

    class BlockRef;

    class JASPERENGINE_API BlockData {
    public:
        BlockData(const BlockData&) = delete;
        BlockData& operator=(const BlockData&) = delete;

        bool sdfIsUniform() const { return sdfValues == nullptr; }
        bool materialIsUniform() const { return materialValues == nullptr; }
        int16_t sdfUniform() const { return sdfValue; }
        uint16_t materialUniform() const { return materialValue; }
        const int16_t* sdf() const { return sdfValues; }
        const uint16_t* material() const { return materialValues; }

        int16_t sdfAt(int index) const { return sdfValues ? sdfValues[index] : sdfValue; }
        uint16_t materialAt(int index) const { return materialValues ? materialValues[index] : materialValue; }

        // For R7's LRU: touched and read only on the main thread, so a job's read view races with nothing
        void stampUse(uint32_t clock) const { useStamp = clock; }
        uint32_t lastUse() const { return useStamp; }

        // The payload R4 counts: 8 KiB per non-uniform channel
        size_t payloadBytes() const;

        // The one way in: copies 4096 values per channel, collapsing a channel whose values are equal
        // (R4). Null when both channels collapse: the caller keeps the two values inline.
        static BlockRef create(const int16_t* sdf, const uint16_t* material);
        // Either pointer null means that channel is uniform with the given value
        static BlockRef createChannels(const int16_t* sdf, int16_t sdfUniform, const uint16_t* material, uint16_t materialUniform);

    private:
        BlockData() = default;
        ~BlockData() = default;

        void retain() const { references.fetch_add(1, std::memory_order_relaxed); }
        void release() const;

        mutable std::atomic<uint32_t> references{1};
        int16_t* sdfValues = nullptr;
        uint16_t* materialValues = nullptr;
        int16_t sdfValue = 0;
        uint16_t materialValue = 0;
        mutable uint32_t useStamp = 0;

        friend class BlockRef;
    };

    // Takes no lock and allocates nothing: what a job's read view is made of
    class BlockRef {
    public:
        BlockRef() = default;
        BlockRef(const BlockRef& other): block(other.block) { if (block) block->retain(); }
        BlockRef(BlockRef&& other) noexcept: block(other.block) { other.block = nullptr; }
        ~BlockRef() { if (block) block->release(); }
        BlockRef& operator=(BlockRef other) noexcept {
            std::swap(block, other.block);
            return *this;
        }

        const BlockData* get() const { return block; }
        const BlockData* operator->() const { return block; }
        explicit operator bool() const { return block != nullptr; }

        // Adopts `raw`'s existing reference, for a pointer that came out of detach()
        static BlockRef adopt(const BlockData* raw) {
            BlockRef ref;
            ref.block = raw;
            return ref;
        }
        // Gives up this handle's reference without releasing it
        const BlockData* detach() {
            const BlockData* raw = block;
            block = nullptr;
            return raw;
        }
        // A new reference to a pointer someone else owns
        static BlockRef share(const BlockData* raw) {
            if (raw) raw->retain();
            return adopt(raw);
        }

    private:
        const BlockData* block = nullptr;
    };

    // A block's content in whichever form the store holds it: both channels inline, or the data. What
    // readers use, so they never see a slot's encoding.
    struct BlockContent {
        BlockRef data;
        int16_t sdfUniform = SDF_AIR;
        uint16_t materialUniform = 0;

        bool isFullyUniform() const { return !data; }
        bool sdfIsUniform() const { return !data || data->sdfIsUniform(); }
        bool materialIsUniform() const { return !data || data->materialIsUniform(); }
        int16_t sdfValueIfUniform() const { return data ? data->sdfUniform() : sdfUniform; }
        uint16_t materialValueIfUniform() const { return data ? data->materialUniform() : materialUniform; }
        int16_t sdfAt(int index) const { return data ? data->sdfAt(index) : sdfUniform; }
        uint16_t materialAt(int index) const { return data ? data->materialAt(index) : materialUniform; }
        size_t payloadBytes() const { return data ? data->payloadBytes() : 0; }
        bool sameAs(const BlockContent& other) const;
    };

    // Collapses the arrays (R4) into a content
    JASPERENGINE_API BlockContent makeContent(const int16_t* sdf, const uint16_t* material);

}

#endif
