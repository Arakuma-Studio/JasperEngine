// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC19: the .jetrees file. A refused file leaves the store as it was, which is what keeps a later
// save from overwriting it (R22).

#include "terrain/TreeStore.h"

#include "zstd.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine::terrain;

namespace {

    TreeRecord makeRecord(float u, float v, uint16_t prototype = 0) {
        TreeRecord record{};
        record.u = u;
        record.v = v;
        record.height = 0.25f;
        record.scale = 1.5f;
        record.rotation[3] = 32767;
        record.prototypeId = prototype;
        record.heightRatio = 128;
        record.flags = TREE_SNAP_TO_TERRAIN;
        std::memset(record.color, 255, 4);
        return record;
    }

    TreeStore makeStore(int count) {
        TreeStore store;
        for (int i = 0; i < count; i++) {
            store.add(makeRecord(static_cast<float>(i) * 1e-4f, 0.5f, static_cast<uint16_t>(i % 3)));
        }
        return store;
    }

    bool zstdCompress(const void* input, size_t inputBytes, std::vector<uint8_t>& output) {
        output.resize(ZSTD_compressBound(inputBytes));
        const size_t written = ZSTD_compress(output.data(), output.size(), input, inputBytes, 3);
        if (ZSTD_isError(written)) return false;
        output.resize(written);
        return true;
    }

    bool zstdDecompress(const void* input, size_t inputBytes, void* output, size_t outputBytes) {
        const size_t written = ZSTD_decompress(output, outputBytes, input, inputBytes);
        return !ZSTD_isError(written) && written == outputBytes;
    }

}

TEST(TreeStore, RoundTripIsByteIdentical) {
    const TreeStore store = makeStore(100);
    std::vector<uint8_t> first;
    store.encode(first);
    EXPECT_EQ(first.size(), TREE_FILE_HEADER_BYTES + 100 * sizeof(TreeRecord));

    TreeStore loaded;
    std::string error;
    ASSERT_TRUE(loaded.decode(first, &error)) << error;
    EXPECT_EQ(loaded.size(), 100u);
    EXPECT_EQ(loaded.nextId(), store.nextId());
    EXPECT_EQ(loaded.contentHash(), store.contentHash());

    std::vector<uint8_t> second;
    loaded.encode(second);
    EXPECT_EQ(first, second);
}

TEST(TreeStore, LargeFilesCompressAndReturn) {
    const TreeStore store = makeStore(5000);
    std::vector<uint8_t> packed;
    store.encode(packed, zstdCompress);
    EXPECT_LT(packed.size(), TREE_FILE_HEADER_BYTES + 5000 * sizeof(TreeRecord));
    uint32_t flags;
    std::memcpy(&flags, packed.data() + 20, 4);
    EXPECT_EQ(flags, TREE_FILE_FLAG_ZSTD);

    TreeStore loaded;
    std::string error;
    EXPECT_FALSE(TreeStore().decode(packed, &error)) << "no decompressor, no records";
    ASSERT_TRUE(loaded.decode(packed, &error, zstdDecompress)) << error;
    EXPECT_EQ(loaded.contentHash(), store.contentHash());

    // small files stay plain, compressor or not
    std::vector<uint8_t> small;
    makeStore(10).encode(small, zstdCompress);
    std::memcpy(&flags, small.data() + 20, 4);
    EXPECT_EQ(flags, 0u);
}

TEST(TreeStore, HeaderIsAsDocumented) {
    const TreeStore store = makeStore(3);
    std::vector<uint8_t> bytes;
    store.encode(bytes);
    EXPECT_EQ(std::memcmp(bytes.data(), "JETR", 4), 0);
    uint32_t value;
    std::memcpy(&value, bytes.data() + 4, 4);
    EXPECT_EQ(value, 1u);
    std::memcpy(&value, bytes.data() + 8, 4);
    EXPECT_EQ(value, 3u);
    std::memcpy(&value, bytes.data() + 12, 4);
    EXPECT_EQ(value, 4u);
    std::memcpy(&value, bytes.data() + 16, 4);
    EXPECT_EQ(value, 3u * 40u);
}

TEST(TreeStore, RejectsTruncatedFile) {
    const TreeStore store = makeStore(50);
    std::vector<uint8_t> bytes;
    store.encode(bytes);

    TreeStore loaded = makeStore(2);
    const uint64_t hash = loaded.contentHash();
    std::string error;

    std::vector<uint8_t> tooShort(bytes.begin(), bytes.begin() + 10);
    EXPECT_FALSE(loaded.decode(tooShort, &error));
    EXPECT_NE(error.find("truncated"), std::string::npos) << error;

    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 40);
    EXPECT_FALSE(loaded.decode(cut, &error));
    EXPECT_NE(error.find("truncated"), std::string::npos) << error;

    EXPECT_EQ(loaded.contentHash(), hash) << "a refused file changes nothing";
}

TEST(TreeStore, RejectsFutureVersion) {
    const TreeStore store = makeStore(5);
    std::vector<uint8_t> bytes;
    store.encode(bytes);
    const uint32_t future = TREE_FILE_VERSION + 1;
    std::memcpy(bytes.data() + 4, &future, 4);

    TreeStore loaded;
    std::string error;
    EXPECT_FALSE(loaded.decode(bytes, &error));
    EXPECT_NE(error.find("version"), std::string::npos) << error;
    EXPECT_TRUE(loaded.empty());
}

TEST(TreeStore, RejectsBadHash) {
    const TreeStore store = makeStore(5);
    std::vector<uint8_t> bytes;
    store.encode(bytes);
    bytes[TREE_FILE_HEADER_BYTES + 7] ^= 0x40;

    TreeStore loaded;
    std::string error;
    EXPECT_FALSE(loaded.decode(bytes, &error));
    EXPECT_NE(error.find("hash"), std::string::npos) << error;
    EXPECT_TRUE(loaded.empty());
}

TEST(TreeStore, RejectsBadMagic) {
    std::vector<uint8_t> bytes;
    makeStore(1).encode(bytes);
    bytes[0] = 'X';
    TreeStore loaded;
    std::string error;
    EXPECT_FALSE(loaded.decode(bytes, &error));
    EXPECT_NE(error.find("magic"), std::string::npos) << error;
}

TEST(TreeStore, IdsAreNeverReused) {
    TreeStore store = makeStore(3);
    const uint32_t last = store.records().back().id;
    EXPECT_TRUE(store.remove(last));
    const uint32_t next = store.add(makeRecord(0.1f, 0.1f));
    EXPECT_GT(next, last);

    // across a save and a load, too
    EXPECT_TRUE(store.remove(next));
    std::vector<uint8_t> bytes;
    store.encode(bytes);
    TreeStore loaded;
    std::string error;
    ASSERT_TRUE(loaded.decode(bytes, &error)) << error;
    EXPECT_GT(loaded.add(makeRecord(0.2f, 0.2f)), next);
}

TEST(TreeStore, CorruptFileIsNotOverwritten) {
    // S03's refusal of lossy saves: what the system reads before it saves over the file
    std::vector<uint8_t> bytes;
    makeStore(4).encode(bytes);
    const std::vector<uint8_t> original = bytes;
    bytes[bytes.size() - 1] ^= 0x01;

    TreeStore store;
    std::string error;
    ASSERT_FALSE(store.decode(bytes, &error));
    EXPECT_TRUE(store.empty()) << "draws no trees";

    // the refusal keeps the file's own next id out of reach, so nothing claims it can save
    EXPECT_EQ(store.nextId(), 1u);
}

TEST(TreeStore, DeltaUndoRestoresTheBytes) {
    TreeStore store = makeStore(10);
    std::vector<uint8_t> before;
    store.encode(before);

    TreeRecord changed = store.records()[4];
    changed.scale = 9.0f;
    const uint32_t removeIds[] = {store.records()[1].id, store.records()[7].id};
    const TreeRecord added[] = {makeRecord(0.9f, 0.9f), makeRecord(0.8f, 0.8f)};
    TreeDelta delta;
    store.apply(removeIds, added, std::span<const TreeRecord>(&changed, 1), &delta);
    EXPECT_EQ(store.size(), 10u);
    EXPECT_EQ(delta.removed.size(), 3u);
    EXPECT_EQ(delta.added.size(), 3u);
    EXPECT_LT(delta.memoryBytes(), 400u);
    std::vector<uint8_t> after;
    store.encode(after);

    store.applyDelta(delta, true);
    std::vector<uint8_t> undone;
    store.encode(undone);
    // the records are back; only next id remembers, since ids are not reused
    EXPECT_EQ(std::vector<uint8_t>(undone.begin() + 32, undone.end()), std::vector<uint8_t>(before.begin() + 32, before.end()));
    uint32_t nextId;
    std::memcpy(&nextId, undone.data() + 12, 4);
    EXPECT_EQ(nextId, delta.nextIdAfter);

    store.applyDelta(delta, false);
    std::vector<uint8_t> redone;
    store.encode(redone);
    EXPECT_EQ(redone, after);
}

TEST(TreeStore, XXHash64MatchesThePublishedVectors) {
    // the reference implementation's values for these inputs
    EXPECT_EQ(treeXXHash64("", 0), 0xEF46DB3751D8E999ull);
    EXPECT_EQ(treeXXHash64("a", 1), 0xD24EC4F1A98C6E5Bull);
    EXPECT_EQ(treeXXHash64("abc", 3), 0x44BC2CF5AD770999ull);
    const std::string quick = "Nobody inspects the spammish repetition";
    EXPECT_EQ(treeXXHash64(quick.data(), quick.size()), 0xFBCEA83C8A378BF1ull);
}
