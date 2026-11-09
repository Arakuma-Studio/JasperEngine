// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_STORE_H
#define VOXEL_STORE_H

// The volume's samples: one flat table of 8-byte slots per level (R4, R5), with block data
// copy-on-write (R6). Only the main thread writes the tables; a job reads through a ReadView, which
// holds its own references, so it sees one version without a lock.

#include "Export.h"
#include "VoxelBlock.h"
#include "VoxelTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace jasperengine::voxel {

    // R33: what an untouched block is, a pure function of its sample positions at any level, thread-safe
    class JASPERENGINE_API BaseSource {
    public:
        virtual ~BaseSource() = default;
        // The samples at origin + (i, j, k) * spacing, in voxels. Writes 4096 values to each array.
        virtual void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const = 0;
        // A block known to be uniform without evaluating it (T2's rangeOver clip): both values, true
        virtual bool evaluateUniform(const Int3& origin, int spacing, int16_t& sdf, uint16_t& material) const {
            (void)origin; (void)spacing; (void)sdf; (void)material;
            return false;
        }
    };

    // The file's blocks, decoded on demand (R7's cold form). Thread-safe over immutable bytes.
    class JASPERENGINE_API ColdSource {
    public:
        virtual ~ColdSource() = default;
        virtual BlockContent decode(uint32_t index) const = 0;
        virtual size_t byteSize() const = 0;
    };

    // R7: one budget over all volumes of a target. Undo memory is charged here too (R29).
    class JASPERENGINE_API MemoryBudget {
    public:
        explicit MemoryBudget(int64_t limitBytes = INT64_MAX): limit(limitBytes) {}
        int64_t used() const { return usedBytes.load(std::memory_order_relaxed); }
        int64_t capacity() const { return limit; }
        void setCapacity(int64_t limitBytes) { limit = limitBytes; }
        void charge(int64_t delta) { usedBytes.fetch_add(delta, std::memory_order_relaxed); }
        bool fits(int64_t delta) const { return used() + delta <= limit; }

    private:
        std::atomic<int64_t> usedBytes{0};
        int64_t limit;
    };

    // One table entry: 8 bytes whatever the content (R4), tagged in the low bits a block's 16-byte
    // alignment leaves free. Copying a hot slot shares the block.
    class JASPERENGINE_API Slot {
    public:
        enum Kind : uint8_t { Hot = 0, Uniform = 1, Base = 2, Cold = 3 };

        Slot() { setUniform(SDF_AIR, 0, false, false); }
        Slot(const Slot& other): bits(other.bits) { retain(); }
        Slot(Slot&& other) noexcept: bits(other.bits) { other.bits = uniformBits(SDF_AIR, 0, false, false); }
        ~Slot() { release(); }
        Slot& operator=(Slot other) noexcept {
            std::swap(bits, other.bits);
            return *this;
        }

        Kind kind() const { return static_cast<Kind>(bits & 3u); }
        // Differs from the base (R76): what an editor file stores
        bool modified() const { return (bits & MODIFIED) != 0; }
        // Changed since the file was loaded (R79): what a delta stores, never evicted (R7)
        bool edited() const { return (bits & EDITED) != 0; }
        int16_t uniformSdf() const { return static_cast<int16_t>(static_cast<uint16_t>(bits >> 16)); }
        uint16_t uniformMaterial() const { return static_cast<uint16_t>(bits >> 32); }
        uint32_t coldIndex() const { return static_cast<uint32_t>(bits >> 32); }
        const BlockData* hotBlock() const { return reinterpret_cast<const BlockData*>(static_cast<uintptr_t>(bits & ~static_cast<uint64_t>(15))); }

        void setUniform(int16_t sdf, uint16_t material, bool isModified, bool isEdited);
        void setBase(bool isModified, bool isEdited);
        void setCold(uint32_t index, bool isModified, bool isEdited);
        void setHot(BlockRef block, bool isModified, bool isEdited);
        void setContent(const BlockContent& content, bool isModified, bool isEdited);

    private:
        static constexpr uint64_t MODIFIED = 4;
        static constexpr uint64_t EDITED = 8;

        static uint64_t uniformBits(int16_t sdf, uint16_t material, bool isModified, bool isEdited);
        void retain() const;
        void release();

        uint64_t bits = 0;
    };
    static_assert(sizeof(Slot) == 8, "R4: a slot is 8 bytes on every target");

    // A block and the state of its slot before or after an edit: the delta R29's undo swaps
    struct SlotChange {
        BlockKey key;
        Slot before;
        Slot after;
    };

    struct MemoryStats {
        int64_t hotBytes = 0;
        int64_t coldBytes = 0;
        int64_t tableBytes = 0;
        int64_t total() const { return hotBytes + coldBytes + tableBytes; }
    };

    class Store;

    // What one mesh job reads (V12): references to 27 blocks at its level and, for masked sides, up to
    // 27 at the next. Whatever it resolves it keeps, so the main thread can publish it afterwards.
    class JASPERENGINE_API ReadView {
    public:
        struct Resolved {
            BlockKey key;
            BlockContent content;
        };

        const VolumeDesc& desc() const { return volume; }
        const BlockKey& center() const { return centerKey; }

        // The R8 rule over this view: an index under 0 clamps, an index past the end reads air in a
        // Closed volume and the nearest edge sample in an Open one. Indices are level samples.
        void fetchSdf(int level, const Int3& first, const Int3& size, int16_t* out) const;
        void fetchMaterial(int level, const Int3& first, const Int3& size, uint16_t* out) const;

        // The contents of the 3x3x3 neighbourhood of a level (0: the block's, 1: the next), for early outs.
        // False when any of them has more than one value or a sign other than the first's.
        bool windowAllOneSign(bool& solid) const;
        // The value when every block of the window has one material
        bool windowOneMaterial(uint16_t& material) const;

        // What this view's fetches resolved (a base evaluation or a cold decode)
        std::vector<Resolved> takeResolved() const;

    private:
        friend class Store;

        struct Cell {
            BlockKey key;
            bool valid = false;           // inside the volume's block grid
            mutable bool resolved = false;
            Slot slot;
            mutable BlockContent content;
        };

        const BlockContent& contentOf(const Cell& cell) const;
        const Cell* cellFor(int group, const BlockKey& key) const;

        VolumeDesc volume;
        BlockKey centerKey;
        std::shared_ptr<const BaseSource> baseSource;
        std::shared_ptr<const ColdSource> coldSource;
        std::array<Cell, 27> own;
        std::array<Cell, 27> next;
        bool hasNext = false;
        mutable std::vector<Resolved> resolvedBlocks;
    };

    class JASPERENGINE_API Store {
    public:
        explicit Store(const VolumeDesc& desc);
        ~Store();
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        // R6: Play gets a copy of the tables, sharing the immutable blocks
        std::unique_ptr<Store> clone() const;

        const VolumeDesc& desc() const { return volume; }
        int levels() const { return volume.levels; }
        // Samples per axis of a level
        Int3 levelSamples(int level) const;
        // Blocks per axis of a level; a coarse block may reach past the end of the volume (R8)
        Int3 blockCount(int level) const;
        size_t slotCount(int level) const { return tables[static_cast<size_t>(level)].size(); }
        bool validKey(const BlockKey& key) const;

        // Bumped by every committed change, which is what a change set's data version is (R17)
        uint64_t version() const { return dataVersion; }
        void bumpVersion() { dataVersion++; }

        void setBase(std::shared_ptr<const BaseSource> source);
        // The same source of truth over new input (R46): no slot changes
        void replaceBase(std::shared_ptr<const BaseSource> source) { baseSource = std::move(source); }
        const std::shared_ptr<const BaseSource>& base() const { return baseSource; }
        // Every slot back to the base, with no block modified
        void resetToBase();
        // Unmodified slots back to the base, for a regenerate with keep_edits (R33)
        void resetUnmodifiedToBase();
        // One unmodified block back to its base (R46: the terrain under it changed)
        void resetBlockToBase(const BlockKey& key);
        // A loaded file's blocks
        void setColdSource(std::shared_ptr<const ColdSource> source);
        const std::shared_ptr<const ColdSource>& cold() const { return coldSource; }
        void setColdSlot(const BlockKey& key, uint32_t index, bool isModified);
        void setLoadedSlot(const BlockKey& key, const BlockContent& content, bool isModified);

        const Slot& slot(const BlockKey& key) const { return tables[static_cast<size_t>(key.level)][slotIndex(key)]; }

        // The block's content, evaluating or decoding it if it has to, without changing the store. Main thread.
        BlockContent readBlock(const BlockKey& key) const;
        // The same, keeping what it resolved (R32: reads resolve cold blocks synchronously). Main thread.
        BlockContent content(const BlockKey& key);

        // The R8 rule; level sample indices. Main thread.
        int16_t sdfAt(int level, int x, int y, int z);
        uint16_t materialAt(int level, int x, int y, int z);

        // sideMask non-zero: the view includes level + 1
        std::shared_ptr<const ReadView> readView(const BlockKey& key, uint8_t sideMask) const;
        // Keeps what a job resolved, unless the slot moved on meanwhile
        void publish(const std::vector<ReadView::Resolved>& resolved);
        void publishOne(const BlockKey& key, const BlockContent& content);

        // A committed write: an edit's new block, keeping the delta's state
        void writeBlock(const BlockKey& key, const BlockContent& content);
        // Undo and redo (R29): swaps the slot to the recorded state
        void applySlot(const BlockKey& key, const Slot& slotValue);
        // A block back to a state saved by a delta
        void writeLoadedDelta(const BlockKey& key, const BlockContent& content);

        // Does the content equal what the base gives? (R76) A store with no base has an air base.
        bool equalsBase(const BlockKey& key, const BlockContent& content) const;
        BlockContent evaluateBase(const BlockKey& key) const;

        // Memory (R7)
        MemoryStats memory() const;
        void setBudget(std::shared_ptr<MemoryBudget> shared);
        const std::shared_ptr<MemoryBudget>& budget() const { return memoryBudget; }
        // Least recently used unmodified hot blocks back to their cold or generator form, until
        // `bytes` are free. Returns the bytes freed.
        int64_t evictUnmodified(int64_t bytes);
        // Would `delta` more bytes fit, after evicting what can be?
        bool makeRoom(int64_t delta);
        // The same question without evicting: what a dry run answers (R27)
        bool wouldFit(int64_t delta) const;

        // Counts for R64's stats
        struct BlockCounts {
            int64_t total = 0, uniform = 0, nonUniform = 0, base = 0, cold = 0, modified = 0;
        };
        BlockCounts countBlocks() const;

        // The blocks a delta file stores (R79)
        std::vector<BlockKey> editedBlocks() const;
        // Forgets which blocks were edited since the load (a load finishes with this)
        void clearEditedFlags();

    private:
        size_t slotIndex(const BlockKey& key) const {
            const Int3& count = counts[static_cast<size_t>(key.level)];
            return static_cast<size_t>(key.x) + static_cast<size_t>(count.x) * (static_cast<size_t>(key.y) + static_cast<size_t>(count.y) * static_cast<size_t>(key.z));
        }
        void setSlot(const BlockKey& key, Slot value);
        static int64_t slotBytes(const Slot& value);
        void addBytes(int64_t delta);

        VolumeDesc volume;
        std::array<Int3, MAX_LEVELS> counts{};
        std::vector<std::vector<Slot>> tables;
        // What a hot slot goes back to when evicted, only for a store loaded from a file
        std::vector<std::vector<uint32_t>> coldIndexOf;
        std::shared_ptr<const BaseSource> baseSource;
        std::shared_ptr<const ColdSource> coldSource;
        std::shared_ptr<MemoryBudget> memoryBudget;
        int64_t hotBytes = 0;
        int64_t tableBytes = 0;
        uint64_t dataVersion = 1;
        mutable uint32_t useClock = 1;
    };

    // Applies R8 to a base block's values in place: a Closed volume's outer layer holds air, and a
    // coarse block's samples past the end read as the rule says, so the block can collapse
    JASPERENGINE_API void lockBaseBlock(const VolumeDesc& desc, const BlockKey& key, int16_t* sdf, uint16_t* material);

}

#endif
