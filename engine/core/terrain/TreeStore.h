// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TREE_STORE_H
#define TREE_STORE_H

// The Terrain's stored trees (T5 R22, F9): records sorted by id, ids never reused, and the .jetrees
// bytes they save to. Pure: reading and writing the file is the caller's (AssetIOService, writeFileAtomic).

#include "Export.h"
#include "terrain/FoliageTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    inline constexpr uint32_t TREE_FILE_MAGIC = 0x5254454Au;    // "JETR", little-endian
    inline constexpr uint32_t TREE_FILE_VERSION = 1;
    inline constexpr size_t TREE_FILE_HEADER_BYTES = 32;
    inline constexpr size_t TREE_FILE_COMPRESS_ABOVE = 64 * 1024;
    inline constexpr uint32_t TREE_FILE_FLAG_ZSTD = 1u;

    // Bytes out or false. The players have no compressor (only zstd_decompress): the editor hands one in
    using TreeCompressFn = bool (*)(const void* input, size_t inputBytes, std::vector<uint8_t>& output);
    // Exactly outputBytes out or false. Handed in so this folder needs no library (T1 R1)
    using TreeDecompressFn = bool (*)(const void* input, size_t inputBytes, void* output, size_t outputBytes);

    // What changed, records as they were and are: undo keeps these, not the file (T5 R52)
    struct TreeDelta {
        std::vector<TreeRecord> removed;    // as they were
        std::vector<TreeRecord> added;      // as they are
        uint32_t nextIdBefore = 0;
        uint32_t nextIdAfter = 0;

        size_t memoryBytes() const { return sizeof(TreeDelta) + (removed.capacity() + added.capacity()) * sizeof(TreeRecord); }
        bool empty() const { return removed.empty() && added.empty() && nextIdBefore == nextIdAfter; }
    };

    class JASPERENGINE_API TreeStore {
    public:
        const std::vector<TreeRecord>& records() const { return treeRecords; }
        size_t size() const { return treeRecords.size(); }
        bool empty() const { return treeRecords.empty(); }
        uint32_t nextId() const { return nextTreeId; }
        // Bumps each time a record changes, for whoever keeps something derived
        uint64_t revision() const { return treeRevision; }

        const TreeRecord* find(uint32_t id) const;

        // Takes the next id and returns it. Past the stored id range it returns 0 and adds nothing
        // (0 is never given: ids start at 1).
        uint32_t add(TreeRecord record);
        bool remove(uint32_t id, TreeRecord* removed = nullptr);
        // By id, which stays
        bool update(const TreeRecord& record);

        // Applies a set of changes through one delta. removed: ids taken away; added: new records
        // (id 0 takes the next id, a kept id puts that record back). Undo is applyDelta(delta, true).
        void apply(std::span<const uint32_t> removed, std::span<const TreeRecord> added, std::span<const TreeRecord> updated, TreeDelta* delta);
        void applyDelta(const TreeDelta& delta, bool undo);

        void clear();

        // xxhash64 of the records, little-endian, as the file stores it
        uint64_t contentHash() const;

        // The file's bytes (F9). A compressor is used above TREE_FILE_COMPRESS_ABOVE, and its result
        // kept only when smaller.
        void encode(std::vector<uint8_t>& out, TreeCompressFn compress = nullptr) const;
        // False with a message naming what is wrong: this store is left untouched
        bool decode(std::span<const uint8_t> bytes, std::string* error, TreeDecompressFn decompress = nullptr);

    private:
        std::vector<TreeRecord> treeRecords;    // sorted by id
        uint32_t nextTreeId = 1;
        uint64_t treeRevision = 0;
    };

    JASPERENGINE_API uint64_t treeXXHash64(const void* data, size_t size, uint64_t seed = 0);

}

#endif // TREE_STORE_H
