// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TreeStore.h"


#include <algorithm>
#include <cstring>

using namespace jasperengine::terrain;

namespace {

    inline uint64_t treeRead64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
    inline uint32_t treeRead32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    inline uint64_t treeRotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

    constexpr uint64_t TREE_P1 = 11400714785074694791ull;
    constexpr uint64_t TREE_P2 = 14029467366897019727ull;
    constexpr uint64_t TREE_P3 = 1609587929392839161ull;
    constexpr uint64_t TREE_P4 = 9650029242287828579ull;
    constexpr uint64_t TREE_P5 = 2870177450012600261ull;

    inline uint64_t treeRound(uint64_t acc, uint64_t input) {
        acc += input * TREE_P2;
        acc = treeRotl64(acc, 31);
        return acc * TREE_P1;
    }

    inline uint64_t treeMerge(uint64_t acc, uint64_t value) {
        acc ^= treeRound(0, value);
        return acc * TREE_P1 + TREE_P4;
    }

    inline void treeWrite32(uint8_t* p, uint32_t value) { std::memcpy(p, &value, 4); }
    inline void treeWrite64(uint8_t* p, uint64_t value) { std::memcpy(p, &value, 8); }

    bool treeIdLess(const TreeRecord& a, const TreeRecord& b) { return a.id < b.id; }

}

// XXH64 (Yann Collet), written from its published specification: the format needs the stated
// algorithm in every player, and zstd's copy is private to its library.
uint64_t jasperengine::terrain::treeXXHash64(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + size;
    uint64_t hash;
    if (size >= 32) {
        const uint8_t* limit = end - 32;
        uint64_t v1 = seed + TREE_P1 + TREE_P2;
        uint64_t v2 = seed + TREE_P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - TREE_P1;
        do {
            v1 = treeRound(v1, treeRead64(p)); p += 8;
            v2 = treeRound(v2, treeRead64(p)); p += 8;
            v3 = treeRound(v3, treeRead64(p)); p += 8;
            v4 = treeRound(v4, treeRead64(p)); p += 8;
        } while (p <= limit);
        hash = treeRotl64(v1, 1) + treeRotl64(v2, 7) + treeRotl64(v3, 12) + treeRotl64(v4, 18);
        hash = treeMerge(hash, v1);
        hash = treeMerge(hash, v2);
        hash = treeMerge(hash, v3);
        hash = treeMerge(hash, v4);
    } else {
        hash = seed + TREE_P5;
    }
    hash += static_cast<uint64_t>(size);
    while (p + 8 <= end) {
        hash ^= treeRound(0, treeRead64(p));
        hash = treeRotl64(hash, 27) * TREE_P1 + TREE_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        hash ^= static_cast<uint64_t>(treeRead32(p)) * TREE_P1;
        hash = treeRotl64(hash, 23) * TREE_P2 + TREE_P3;
        p += 4;
    }
    while (p < end) {
        hash ^= static_cast<uint64_t>(*p) * TREE_P5;
        hash = treeRotl64(hash, 11) * TREE_P1;
        p++;
    }
    hash ^= hash >> 33;
    hash *= TREE_P2;
    hash ^= hash >> 29;
    hash *= TREE_P3;
    hash ^= hash >> 32;
    return hash;
}

const TreeRecord* TreeStore::find(uint32_t id) const {
    TreeRecord key{};
    key.id = id;
    const auto it = std::lower_bound(treeRecords.begin(), treeRecords.end(), key, treeIdLess);
    return (it != treeRecords.end() && it->id == id) ? &*it : nullptr;
}

uint32_t TreeStore::add(TreeRecord record) {
    if (nextTreeId >= TREE_RUNTIME_ID_BASE) {
        return 0;
    }
    record.id = nextTreeId++;
    // ids only grow: this is the end
    treeRecords.push_back(record);
    treeRevision++;
    return record.id;
}

bool TreeStore::remove(uint32_t id, TreeRecord* removed) {
    TreeRecord key{};
    key.id = id;
    const auto it = std::lower_bound(treeRecords.begin(), treeRecords.end(), key, treeIdLess);
    if (it == treeRecords.end() || it->id != id) {
        return false;
    }
    if (removed) *removed = *it;
    treeRecords.erase(it);
    treeRevision++;
    return true;
}

bool TreeStore::update(const TreeRecord& record) {
    const auto it = std::lower_bound(treeRecords.begin(), treeRecords.end(), record, treeIdLess);
    if (it == treeRecords.end() || it->id != record.id) {
        return false;
    }
    *it = record;
    treeRevision++;
    return true;
}

void TreeStore::apply(std::span<const uint32_t> removed, std::span<const TreeRecord> added, std::span<const TreeRecord> updated, TreeDelta* delta) {
    if (delta) delta->nextIdBefore = nextTreeId;

    if (!removed.empty()) {
        std::vector<uint32_t> ids(removed.begin(), removed.end());
        std::sort(ids.begin(), ids.end());
        size_t keep = 0;
        size_t idIndex = 0;
        for (size_t i = 0; i < treeRecords.size(); i++) {
            while (idIndex < ids.size() && ids[idIndex] < treeRecords[i].id) idIndex++;
            if (idIndex < ids.size() && ids[idIndex] == treeRecords[i].id) {
                if (delta) delta->removed.push_back(treeRecords[i]);
                continue;
            }
            if (keep != i) treeRecords[keep] = treeRecords[i];
            keep++;
        }
        treeRecords.resize(keep);
    }

    for (const TreeRecord& record : updated) {
        const TreeRecord* existing = find(record.id);
        if (!existing) continue;
        if (delta) {
            delta->removed.push_back(*existing);
            delta->added.push_back(record);
        }
        update(record);
    }

    bool sorted = true;
    for (const TreeRecord& record : added) {
        TreeRecord stored = record;
        if (stored.id == 0) {
            if (nextTreeId >= TREE_RUNTIME_ID_BASE) break;
            stored.id = nextTreeId++;
        } else if (stored.id >= nextTreeId) {
            nextTreeId = stored.id + 1;
        }
        if (!treeRecords.empty() && stored.id < treeRecords.back().id) sorted = false;
        treeRecords.push_back(stored);
        if (delta) delta->added.push_back(stored);
    }
    if (!sorted) {
        std::sort(treeRecords.begin(), treeRecords.end(), treeIdLess);
    }

    if (delta) delta->nextIdAfter = nextTreeId;
    treeRevision++;
}

void TreeStore::applyDelta(const TreeDelta& delta, bool undo) {
    const std::vector<TreeRecord>& take = undo ? delta.added : delta.removed;
    const std::vector<TreeRecord>& put = undo ? delta.removed : delta.added;

    std::vector<uint32_t> ids;
    ids.reserve(take.size());
    for (const TreeRecord& record : take) ids.push_back(record.id);
    std::sort(ids.begin(), ids.end());

    size_t keep = 0;
    for (size_t i = 0; i < treeRecords.size(); i++) {
        if (std::binary_search(ids.begin(), ids.end(), treeRecords[i].id)) continue;
        if (keep != i) treeRecords[keep] = treeRecords[i];
        keep++;
    }
    treeRecords.resize(keep);
    treeRecords.insert(treeRecords.end(), put.begin(), put.end());
    std::sort(treeRecords.begin(), treeRecords.end(), treeIdLess);
    // undo keeps ids from being reused: a redo puts the same ids back, and only those
    nextTreeId = std::max(nextTreeId, undo ? delta.nextIdBefore : delta.nextIdAfter);
    treeRevision++;
}

void TreeStore::clear() {
    if (treeRecords.empty()) return;
    treeRecords.clear();
    treeRevision++;
}

uint64_t TreeStore::contentHash() const {
    return treeXXHash64(treeRecords.data(), treeRecords.size() * sizeof(TreeRecord));
}

void TreeStore::encode(std::vector<uint8_t>& out, TreeCompressFn compress) const {
    const size_t recordBytes = treeRecords.size() * sizeof(TreeRecord);
    std::vector<uint8_t> packed;
    bool useCompressed = false;
    if (compress && recordBytes > TREE_FILE_COMPRESS_ABOVE && compress(treeRecords.data(), recordBytes, packed) && packed.size() < recordBytes) {
        useCompressed = true;
    }
    const size_t payload = useCompressed ? packed.size() : recordBytes;

    out.assign(TREE_FILE_HEADER_BYTES + payload, 0);
    uint8_t* header = out.data();
    treeWrite32(header + 0, TREE_FILE_MAGIC);
    treeWrite32(header + 4, TREE_FILE_VERSION);
    treeWrite32(header + 8, static_cast<uint32_t>(treeRecords.size()));
    treeWrite32(header + 12, nextTreeId);
    treeWrite32(header + 16, static_cast<uint32_t>(payload));
    treeWrite32(header + 20, useCompressed ? TREE_FILE_FLAG_ZSTD : 0u);
    treeWrite64(header + 24, contentHash());
    if (payload > 0) {
        std::memcpy(header + TREE_FILE_HEADER_BYTES, useCompressed ? static_cast<const void*>(packed.data()) : static_cast<const void*>(treeRecords.data()), payload);
    }
}

bool TreeStore::decode(std::span<const uint8_t> bytes, std::string* error, TreeDecompressFn decompress) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };

    if (bytes.size() < TREE_FILE_HEADER_BYTES) {
        return fail("truncated: " + std::to_string(bytes.size()) + " bytes is less than the header");
    }
    const uint8_t* header = bytes.data();
    if (treeRead32(header) != TREE_FILE_MAGIC) {
        return fail("not a tree file (bad magic)");
    }
    const uint32_t version = treeRead32(header + 4);
    if (version != TREE_FILE_VERSION) {
        return fail("version " + std::to_string(version) + " is newer than this engine's " + std::to_string(TREE_FILE_VERSION));
    }
    const uint32_t count = treeRead32(header + 8);
    const uint32_t nextId = treeRead32(header + 12);
    const uint32_t payload = treeRead32(header + 16);
    const uint32_t flags = treeRead32(header + 20);
    const uint64_t hash = treeRead64(header + 24);

    if (bytes.size() != TREE_FILE_HEADER_BYTES + payload) {
        return fail("truncated: the header says " + std::to_string(payload) + " payload bytes, the file has " + std::to_string(bytes.size() - TREE_FILE_HEADER_BYTES));
    }

    const size_t recordBytes = static_cast<size_t>(count) * sizeof(TreeRecord);
    std::vector<TreeRecord> records(count);
    const uint8_t* source = header + TREE_FILE_HEADER_BYTES;
    if (flags & TREE_FILE_FLAG_ZSTD) {
        if (!decompress) {
            return fail("compressed, and this reader was given no decompressor");
        }
        if (!decompress(source, payload, records.data(), recordBytes)) {
            return fail("corrupt: the compressed records do not decompress to the stated count");
        }
    } else {
        if (payload != recordBytes) {
            return fail("corrupt: the stated count does not fit the payload");
        }
        if (recordBytes > 0) std::memcpy(static_cast<void*>(records.data()), source, recordBytes);
    }

    if (treeXXHash64(records.data(), recordBytes) != hash) {
        return fail("corrupt: the records' hash does not match");
    }
    for (size_t i = 1; i < records.size(); i++) {
        if (records[i - 1].id >= records[i].id) {
            return fail("corrupt: the records are not sorted by id");
        }
    }
    if (!records.empty() && records.back().id >= nextId) {
        return fail("corrupt: next id is not past the last record");
    }

    treeRecords = std::move(records);
    nextTreeId = nextId;
    treeRevision++;
    return true;
}
