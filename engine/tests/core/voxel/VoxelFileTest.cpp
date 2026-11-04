// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R74-R76, R79 (AC47, AC48, AC51, AC6): the file format and the codec

#include "VoxelTestSupport.h"
#include "voxel/VoxelCodec.h"
#include "voxel/VoxelFile.h"

#include <gtest/gtest.h>

using namespace voxeltest;

namespace {

    std::unique_ptr<Store> reopen(const std::vector<uint8_t>& bytes, std::shared_ptr<const BaseSource> base = nullptr) {
        const VolumeFileResult info = readVolumeFileInfo(bytes);
        EXPECT_TRUE(info.ok) << info.message;
        auto store = std::make_unique<Store>(info.info.desc());
        if (base) store->setBase(std::move(base));
        const VolumeFileResult result = loadVolumeFile(bytes, *store);
        EXPECT_TRUE(result.ok) << result.message;
        return store;
    }

    void dressUp(Store& store) {
        store.setBase(wavyBase(30.0));
        const EditOp ops[] = {digOp(sphereShape(32.0, 30.0, 32.0, 6.0)), addOp(sphereShape(16.0, 40.0, 16.0, 5.0), 3)};
        ASSERT_TRUE(applyEdit(store, ops).ok);
    }

}

TEST(VoxelCodec, Drle16RoundTrip) {
    std::mt19937 random(17);
    uint16_t input[BLOCK_SAMPLES];
    uint16_t output[BLOCK_SAMPLES];
    for (const int pattern : {0, 1, 2, 3}) {
        for (int i = 0; i < BLOCK_SAMPLES; i++) {
            switch (pattern) {
                case 0: input[i] = 7; break;
                case 1: input[i] = static_cast<uint16_t>(random()); break;
                case 2: input[i] = static_cast<uint16_t>(i / 100); break;
                default: input[i] = static_cast<uint16_t>(random() % 16 == 0 ? random() : 0); break;
            }
        }
        std::vector<uint8_t> bytes;
        drle16Encode(input, BLOCK_SAMPLES, bytes);
        ASSERT_TRUE(drle16Decode(bytes.data(), bytes.size(), output, BLOCK_SAMPLES));
        EXPECT_EQ(std::memcmp(input, output, sizeof(input)), 0);
        if (pattern == 0) {
            EXPECT_LT(bytes.size(), 64u);
        }
        // A short stream and a long one both fail, whatever they hold
        EXPECT_FALSE(drle16Decode(bytes.data(), bytes.size() - 1, output, BLOCK_SAMPLES));
        bytes.push_back(0);
        EXPECT_FALSE(drle16Decode(bytes.data(), bytes.size(), output, BLOCK_SAMPLES));
    }
}

TEST(VoxelFile, RoundTripCanonicalBytes) {
    Store store(desc(64, 64, 64, 3));
    dressUp(store);
    SaveOptions options;
    options.slotCount = 4;
    options.generatorJson = "{\"kind\":\"empty\"}";
    const std::vector<uint8_t> first = saveVolumeFile(store, options);
    const uint64_t hash = contentHash(store);
    auto loaded = reopen(first, store.base());
    EXPECT_EQ(contentHash(*loaded), hash);
    for (int level = 1; level < 3; level++) {
        const Int3 samples = loaded->levelSamples(level);
        for (int i = 0; i < samples.x; i++) {
            EXPECT_EQ(loaded->sdfAt(level, i, i, i), store.sdfAt(level, i, i, i));
        }
    }
    // Saved again, byte for byte, even with blocks now hot, cold or resolved
    const std::vector<uint8_t> second = saveVolumeFile(*loaded, options);
    EXPECT_EQ(first, second);
    const VolumeFileResult info = readVolumeFileInfo(first);
    EXPECT_EQ(info.info.generatorJson, options.generatorJson);
    EXPECT_EQ(info.info.levels, 3);
    EXPECT_EQ(info.info.slotCount, 4);
}

TEST(VoxelFile, LittleEndianExplicit) {
    // A tiny file's bytes, written out: the format, whatever the host
    Store store(desc(16, 16, 16, 1, BoundsMode::Open, 2.0f));
    const EditOp op = addOp(sphereShape(16.0, 16.0, 16.0, 4.0), 1);
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
    SaveOptions options;
    options.slotCount = 2;
    const std::vector<uint8_t> file = saveVolumeFile(store, options);
    const uint8_t header[] = {
        'J', 'V', 'O', 'X',
        1, 0,                       // version
        64, 0,                      // header bytes
        4, 0, 0, 0,                 // flags: open_bounds
        1, 0, 1, 0, 1, 0,           // dimensions in blocks
        1,                          // levels
        2,                          // slots
        0, 0, 0, 0x40,              // voxel size 2.0
        0, 0, 0, 0, 0, 0, 0, 0,     // base hash
        64, 0, 0, 0, 0, 0, 0, 0,    // generator: at 64, 0 bytes
        64, 0, 0, 0, 0, 0, 0, 0,    // terrain cut: at 64, 0 bytes
        64, 0, 0, 0, 1, 0, 0, 0,    // directory: at 64, 1 entry
        92, 0, 0, 0,                // payload offset
    };
    ASSERT_GT(file.size(), sizeof(header));
    EXPECT_EQ(std::memcmp(file.data(), header, sizeof(header)), 0);
    // The entry: level, codecs, flags, x y z
    EXPECT_EQ(file[64], 0);
    EXPECT_EQ(file[67], 1);    // modified
    EXPECT_EQ(file[68], 0);
    EXPECT_EQ(volumeFileHash(file), fnv1a(file.data(), file.size() - 8));
}

TEST(VoxelFile, RefusesNewerMajor) {
    Store store(desc(16, 16, 16));
    std::vector<uint8_t> file = saveVolumeFile(store, SaveOptions{});
    file[4] = 9;
    const uint64_t rehash = fnv1a(file.data(), file.size() - 8);
    for (int i = 0; i < 8; i++) file[file.size() - 8 + static_cast<size_t>(i)] = static_cast<uint8_t>(rehash >> (8 * i));
    const std::vector<uint8_t> untouched = file;
    const VolumeFileResult result = readVolumeFileInfo(file);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, FileError::NewerVersion);
    EXPECT_NE(result.message.find("version 9"), std::string::npos);
    EXPECT_NE(result.message.find("version 1"), std::string::npos);
    EXPECT_EQ(file, untouched);
}

TEST(VoxelFile, ModifiedOnly) {
    // R76: one dig in a 256^3 base stores little, and editing back to base stores nothing
    Store store(desc(256, 256, 256, 4));
    store.setBase(wavyBase(120.0));
    SaveOptions options;
    const std::vector<uint8_t> empty = saveVolumeFile(store, options);
    EXPECT_LT(empty.size(), 256u);
    EditOptions edit;
    edit.captureDelta = true;
    const EditOp op = digOp(sphereShape(128.0, 120.0, 128.0, 6.0));
    EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1), edit);
    ASSERT_TRUE(result.ok);
    const std::vector<uint8_t> dug = saveVolumeFile(store, options);
    EXPECT_LT(dug.size(), 200u * 1024u);
    EXPECT_GT(dug.size(), empty.size());
    applyDelta(store, result.delta, true);
    EXPECT_EQ(saveVolumeFile(store, options), empty);
    // Back to base by a further edit, not by undo: the block is written but equals the base
    EditOptions plain;
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1), plain).ok);
    for (const SlotChange& change : result.delta) {
        store.applySlot(change.key, change.before);
    }
    EXPECT_EQ(saveVolumeFile(store, options), empty);
}

TEST(VoxelFile, TooLargeDoesNotLoad) {
    Store store(desc(64, 64, 64));
    dressUp(store);
    const std::vector<uint8_t> bytes = saveVolumeFile(store, SaveOptions{});
    Store target(desc(64, 64, 64));
    const VolumeFileResult result = loadVolumeFile(bytes, target, nullptr, static_cast<int64_t>(bytes.size()) - 1);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, FileError::TooLarge);
}

TEST(VoxelFile, ColdBlocksDecodeOnDemandAndEvictBack) {
    Store store(desc(64, 64, 64));
    dressUp(store);
    const uint64_t hash = contentHash(store);
    const std::vector<uint8_t> bytes = saveVolumeFile(store, SaveOptions{});
    auto loaded = reopen(bytes, store.base());
    const BlockKey key{0, 2, 1, 2};
    EXPECT_EQ(loaded->slot(key).kind(), Slot::Cold);
    loaded->content(key);
    EXPECT_EQ(loaded->slot(key).kind(), Slot::Hot);
    EXPECT_GT(loaded->evictUnmodified(1), 0);
    EXPECT_EQ(loaded->slot(key).kind(), Slot::Cold);
    EXPECT_EQ(contentHash(*loaded), hash);
}

TEST(VoxelFile, CookedHasEveryBlockAndNoGenerator) {
    Store store(desc(64, 64, 64, 2));
    dressUp(store);
    const uint64_t hash = contentHash(store);
    SaveOptions options;
    options.mode = SaveOptions::Mode::Cooked;
    options.generatorJson = "{\"kind\":\"terrain\"}";
    const std::vector<uint8_t> file = saveVolumeFile(store, options);
    const VolumeFileResult info = readVolumeFileInfo(file);
    ASSERT_TRUE(info.ok);
    EXPECT_TRUE(info.info.flags & VOXEL_FILE_COOKED);
    EXPECT_TRUE(info.info.generatorJson.empty());
    // No base needed to load it
    auto baked = reopen(file);
    EXPECT_EQ(contentHash(*baked), hash);
    EXPECT_EQ(baked->sdfAt(1, 10, 10, 10), store.sdfAt(1, 10, 10, 10));
    SaveOptions kept = options;
    kept.keepGenerator = true;
    EXPECT_EQ(readVolumeFileInfo(saveVolumeFile(store, kept)).info.generatorJson, options.generatorJson);
}

TEST(VoxelRuntime, SaveLoadChanges) {
    Store base(desc(64, 64, 64, 2));
    base.setBase(wavyBase(30.0));
    const std::vector<uint8_t> baseFile = saveVolumeFile(base, SaveOptions{});
    const uint64_t baseHash = volumeFileHash(baseFile);
    auto play = reopen(baseFile, base.base());
    const EditOp op = digOp(sphereShape(32.0, 30.0, 32.0, 7.0));
    ASSERT_TRUE(applyEdit(*play, std::span<const EditOp>(&op, 1)).ok);
    const uint64_t hash = contentHash(*play);

    SaveOptions save;
    save.mode = SaveOptions::Mode::Delta;
    save.baseHash = baseHash;
    const std::vector<uint8_t> delta = saveVolumeFile(*play, save);
    const VolumeFileResult info = readVolumeFileInfo(delta);
    ASSERT_TRUE(info.ok);
    EXPECT_TRUE(info.info.flags & VOXEL_FILE_DELTA);

    auto fresh = reopen(baseFile, base.base());
    EXPECT_NE(contentHash(*fresh), hash);
    const VolumeFileResult loaded = loadDeltaFile(delta, *fresh, baseHash);
    ASSERT_TRUE(loaded.ok) << loaded.message;
    EXPECT_EQ(contentHash(*fresh), hash);

    // A changed base refuses, and all or nothing means nothing moved
    auto other = reopen(baseFile, base.base());
    const uint64_t otherHash = contentHash(*other);
    const VolumeFileResult refused = loadDeltaFile(delta, *other, baseHash + 1);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error, FileError::BaseChanged);
    EXPECT_EQ(contentHash(*other), otherHash);
}

TEST(VoxelRuntime, DeltaUsesDrle16Only) {
    Store store(desc(64, 64, 64));
    dressUp(store);
    SaveOptions save;
    save.mode = SaveOptions::Mode::Delta;
    const std::vector<uint8_t> delta = saveVolumeFile(store, save);
    const VolumeFileResult info = readVolumeFileInfo(delta);
    ASSERT_TRUE(info.ok);
    ASSERT_GT(info.info.entryCount, 0u);
    for (uint32_t i = 0; i < info.info.entryCount; i++) {
        const uint8_t* entry = delta.data() + 64 + i * VOXEL_FILE_DIRECTORY_ENTRY_BYTES;
        EXPECT_TRUE(entry[1] == 0 || entry[1] == static_cast<uint8_t>(VoxelCodec::Drle16));
        EXPECT_TRUE(entry[2] == 0 || entry[2] == static_cast<uint8_t>(VoxelCodec::Drle16));
    }
}

TEST(VoxelFile, TerrainCutRoundTrip) {
    const uint8_t mask[] = {0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    const TerrainCutRuns runs = TerrainCutRuns::fromMask(mask, 4, 3);
    EXPECT_EQ(runs.runs.size(), 2u);
    EXPECT_EQ(runs.toMask(), std::vector<uint8_t>(mask, mask + 12));
    Store store(desc(16, 16, 16));
    SaveOptions options;
    options.terrainCut = runs;
    const std::vector<uint8_t> file = saveVolumeFile(store, options);
    const VolumeFileResult info = readVolumeFileInfo(file);
    ASSERT_TRUE(info.ok);
    EXPECT_EQ(info.info.terrainCut.toMask(), std::vector<uint8_t>(mask, mask + 12));
}

TEST(VoxelFileSlow, FuzzTruncateAndFlip) {
    Store store(desc(32, 32, 32, 2));
    store.setBase(sphereBase(16.0, 16.0, 16.0, 10.0));
    const EditOp op = digOp(sphereShape(16.0, 16.0, 16.0, 4.0));
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
    SaveOptions options;
    options.generatorJson = "{}";
    const std::vector<uint8_t> good = saveVolumeFile(store, options);
    ASSERT_TRUE(readVolumeFileInfo(good).ok);
    std::mt19937 random(23);
    int rejected = 0;
    for (int i = 0; i < 10000; i++) {
        std::vector<uint8_t> file = good;
        const int kind = i % 3;
        if (kind == 0) {
            file.resize(random() % file.size());
        } else {
            const int flips = 1 + static_cast<int>(random() % 8);
            for (int f = 0; f < flips; f++) file[random() % file.size()] ^= static_cast<uint8_t>(1u << (random() % 8));
        }
        if (kind == 2 && file.size() >= 8) {
            // Past the check too: the structure itself has to hold up
            const uint64_t rehash = fnv1a(file.data(), file.size() - 8);
            for (int b = 0; b < 8; b++) file[file.size() - 8 + static_cast<size_t>(b)] = static_cast<uint8_t>(rehash >> (8 * b));
        }
        const VolumeFileResult info = readVolumeFileInfo(file);
        if (!info.ok) {
            EXPECT_NE(info.error, FileError::None);
            rejected++;
            continue;
        }
        // It parsed: every block must read without leaving the bytes
        if (info.info.blocks.x > 8 || info.info.blocks.y > 8 || info.info.blocks.z > 8) continue;
        Store target(info.info.desc());
        const VolumeFileResult loaded = loadVolumeFile(file, target);
        if (!loaded.ok) continue;
        contentHash(target);
    }
    EXPECT_GT(rejected, 6000);
}

TEST(VoxelFile, LoadsV1Fixture) {
    // Generated once by the code of the format's first version; never regenerate it. A format change
    // gets a new fixture and a migration.
    const uint8_t fixture[] = {
#include "VoxelFileV1Fixture.inc"
    };
    const std::span<const uint8_t> bytes(fixture, sizeof(fixture));
    const VolumeFileResult info = readVolumeFileInfo(bytes);
    ASSERT_TRUE(info.ok) << info.message;
    EXPECT_EQ(info.info.version, 1);
    EXPECT_EQ(info.info.levels, 2);
    EXPECT_EQ(info.info.slotCount, 3);
    EXPECT_EQ(info.info.generatorJson, "{\"kind\":\"empty\"}");
    Store store(info.info.desc());
    ASSERT_TRUE(loadVolumeFile(bytes, store).ok);
    EXPECT_LT(store.sdfAt(0, 16, 16, 16), 0);
    EXPECT_GT(store.sdfAt(0, 4, 4, 4), 0);
    EXPECT_EQ(contentHash(store), 0x0a82fe9ed6fcdcbeull) << std::hex << contentHash(store);
}
