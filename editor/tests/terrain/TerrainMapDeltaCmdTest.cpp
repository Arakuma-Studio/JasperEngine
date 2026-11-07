// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Delta undo (T1 R24-R28; AC19, AC20, AC62): XOR tiles, the same operation both ways, refused on a map
// it was not cut from, with the memory R26 states. Structural steps keep compressed snapshots.

#include "TerrainProjectFixture.h"
#include "component/TerrainComponent.h"

#include "command/type/TerrainMapDeltaCmd.h"
#include "command/type/TerrainTextureEditCmd.h"
#include "window/TerrainEditWindow.h"

#include <algorithm>
#include <random>
#include <utility>

using namespace terrainprojecttest;

namespace {

    class TerrainMapDeltaCmdTest : public TerrainProjectFixture {
    protected:
        // Runs op into a step applied by the history's execute
        TerrainMapDeltaCmd* stepOf(terrain::OverlayMapStore& overlay, const std::string& verb = {}) {
            auto* delta = new TerrainMapDeltaCmd(project.get(), scene->id, terrainEntity, terrainEntity, overlay, false, verb);
            overlay.discard();
            EXPECT_TRUE(history()->addCommandNoMerge(delta));
            return delta;
        }

        void expectBitExact(const std::vector<terrain::MapId>& maps, const std::vector<std::vector<unsigned char>>& before) {
            std::vector<std::vector<unsigned char>> after;
            for (const terrain::MapId& map : maps) after.push_back(bytesOf(map));
            EXPECT_NE(after, before);
            for (int round = 0; round < 3; round++) {
                ASSERT_TRUE(history()->undo());
                for (size_t i = 0; i < maps.size(); i++) EXPECT_EQ(bytesOf(maps[i]), before[i]) << "round " << round << " map " << i;
                ASSERT_TRUE(history()->redo());
                for (size_t i = 0; i < maps.size(); i++) EXPECT_EQ(bytesOf(maps[i]), after[i]) << "round " << round << " map " << i;
            }
        }
    };

}

TEST_F(TerrainMapDeltaCmdTest, UndoRedoAreBitExact) {
    // 8-bit and 16-bit heights
    for (const ColorFormat format : {ColorFormat::RED, ColorFormat::RED16}) {
        giveFileMap(TerrainMapRef(TerrainMapTarget::HeightMap), 96, format, 1, [](size_t i) { return (i * 7) % 251; });
        const std::vector<unsigned char> before = bytesOf(HEIGHT_MAP);
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(4.0f)).ok);
        stepOf(overlay);
        expectBitExact({HEIGHT_MAP}, {before});
        ASSERT_TRUE(history()->undo());
    }

    // RGBA8 blend and R8 density, one step for the two maps: its name is the first map's
    giveFileMap(TerrainMapRef(TerrainMapTarget::BlendMap, 0), 80, ColorFormat::RGBA, 4, [](size_t i) { return (i % 4) == 3 ? 255 : (i * 3) % 200; });
    giveFileMap(TerrainMapRef(TerrainMapTarget::DensityMap, 0), 70, ColorFormat::RED, 1, [](size_t i) { return (i * 11) % 256; });
    giveHeight16(64);
    const std::vector<std::vector<unsigned char>> before = {bytesOf(HEIGHT_MAP), bytesOf(BLEND_MAP), bytesOf(DENSITY_MAP)};
    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(2.0f)).ok);
        terrain::Op paint;
        paint.type = terrain::OpType::PaintLayer;
        paint.layer = 1;
        paint.shape = raiseOp(0.0f).shape;
        ASSERT_TRUE(runOn(overlay, paint).ok);
        terrain::Op density;
        density.type = terrain::OpType::EraseDensity;
        density.layer = 0;
        density.shape = raiseOp(0.0f).shape;
        ASSERT_TRUE(runOn(overlay, density).ok);
        TerrainMapDeltaCmd* step = stepOf(overlay);
        EXPECT_NE(step->describe().find("Sculpt Terrain"), std::string::npos) << step->describe();
    }
    expectBitExact({HEIGHT_MAP, BLEND_MAP, DENSITY_MAP}, before);
}

TEST_F(TerrainMapDeltaCmdTest, RefusesAfterMapChanged) {
    giveHeight16(64);
    const std::vector<unsigned char> before = bytesOf(HEIGHT_MAP);
    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(4.0f)).ok);
        stepOf(overlay);
    }
    const std::vector<unsigned char> after = bytesOf(HEIGHT_MAP);
    ASSERT_NE(after, before);

    // A hot reload: the entry's generation goes up (S05 R13)
    const std::string id = terrain().heightMap.getId();
    TextureDataPool::replace(id, TextureDataPool::get(id));

    const size_t position = history()->position();
    EXPECT_FALSE(history()->undo()) << "no XOR into other data";
    EXPECT_EQ(history()->position(), position);
    EXPECT_NE(history()->lastRefusal().find("The map changed outside this history"), std::string::npos) << history()->lastRefusal();
    EXPECT_EQ(bytesOf(HEIGHT_MAP), after);
}

TEST_F(TerrainMapDeltaCmdTest, RefusesAMapOfAnotherSize) {
    giveHeight16(64);
    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(4.0f)).ok);
        stepOf(overlay);
    }
    giveHeight16(32);
    EXPECT_FALSE(history()->undo());
    EXPECT_NE(history()->lastRefusal().find("The map changed outside this history"), std::string::npos) << history()->lastRefusal();
}

TEST_F(TerrainMapDeltaCmdTest, CompressedAndRawTilesBothWork) {
    giveHeight16(128);
    const std::vector<unsigned char> before = bytesOf(HEIGHT_MAP);
    TerrainMapDeltaCmd* step = nullptr;
    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        std::mt19937 random(7);
        std::vector<uint8_t> noisy(static_cast<size_t>(64) * 64 * 2);
        for (uint8_t& byte : noisy) byte = static_cast<uint8_t>(random());
        ASSERT_TRUE(overlay.writeBytes(HEIGHT_MAP, terrain::TexelRect{0, 0, 64, 64}, noisy).ok);
        // a gentle change: a low byte moved by one
        std::vector<uint8_t> rect(static_cast<size_t>(64) * 64 * 2);
        for (int y = 0; y < 64; y++) {
            std::copy_n(before.data() + (static_cast<size_t>(y + 64) * 128) * 2, 128, rect.data() + static_cast<size_t>(y) * 128);
        }
        for (size_t i = 0; i < rect.size(); i += 2) rect[i] = static_cast<uint8_t>(rect[i] + 1);
        ASSERT_TRUE(overlay.writeBytes(HEIGHT_MAP, terrain::TexelRect{0, 64, 64, 64}, rect).ok);
        step = stepOf(overlay);
    }
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->tileCount(), 2u);
    EXPECT_EQ(step->rawTileCount(), 1u) << "the noise stays raw, the gentle tile compresses";
    expectBitExact({HEIGHT_MAP}, {before});
}

TEST_F(TerrainMapDeltaCmdTest, AnUnchangedTileIsDropped) {
    giveHeight16(128);
    EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
    terrain::OverlayMapStore overlay(store);
    // touched and put back: the overlay holds the tile, the step has nothing to do
    std::vector<float> values(static_cast<size_t>(64) * 64);
    ASSERT_TRUE(overlay.read(HEIGHT_MAP, terrain::TexelRect{0, 0, 64, 64}, values));
    std::vector<float> moved = values;
    moved[0] += 0.25f;
    ASSERT_TRUE(overlay.write(HEIGHT_MAP, terrain::TexelRect{0, 0, 64, 64}, moved).ok);
    ASSERT_TRUE(overlay.write(HEIGHT_MAP, terrain::TexelRect{0, 0, 64, 64}, values).ok);
    ASSERT_EQ(overlay.tileCount(), 1u);
    TerrainMapDeltaCmd step(project.get(), scene->id, terrainEntity, terrainEntity, overlay, false);
    EXPECT_TRUE(step.empty());
    EXPECT_TRUE(step.isNoOp());
}

// AC20 (R26): at most the map bytes of the changed tiles plus 32 bytes a tile, never two copies
TEST_F(TerrainMapDeltaCmdTest, MemoryBound) {
    constexpr int N = 256;
    constexpr size_t TILE_BYTES = 64 * 64 * 2;
    giveHeight16(N);
    EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
    terrain::OverlayMapStore overlay(store);

    // the worst case: every byte of nine tiles changes to noise, which does not compress
    std::mt19937 random(11);
    const terrain::TexelRect rect{0, 0, 192, 192};
    std::vector<uint8_t> noise(static_cast<size_t>(rect.area()) * 2);
    for (uint8_t& byte : noise) byte = static_cast<uint8_t>(random());
    ASSERT_TRUE(overlay.writeBytes(HEIGHT_MAP, rect, noise).ok);
    ASSERT_EQ(overlay.tileCount(), 9u);

    // the stroke's working memory: tiles * 64^2 * (4 * channels + bytesPerTexel)
    EXPECT_LE(overlay.memoryBytes(), 9u * 64 * 64 * (4 * 1 + 2));

    TerrainMapDeltaCmd step(project.get(), scene->id, terrainEntity, terrainEntity, overlay, false);
    ASSERT_EQ(step.tileCount(), 9u);
    const size_t fixed = 1024;  // the step's own fields and the map's path
    EXPECT_LE(step.memoryBytes(), fixed + 9u * (TILE_BYTES + 32u));
    EXPECT_GE(step.memoryBytes(), 9u * TILE_BYTES) << "it counts what it holds";
}

// AC62 (R28): create, resize and convert keep snapshots, compressed, and undo gives equal bytes
TEST_F(TerrainMapDeltaCmdTest, StructuralStepsCompressAndRestore) {
    const TerrainMapRef heightRef(TerrainMapTarget::HeightMap);
    Texture& map = terrain().heightMap;

    // create
    TerrainMapSnapshot empty = TerrainMapUtils::captureSnapshot(project.get(), map, true);
    TerrainMapSnapshot created = empty;
    created.empty = false;
    created.path = TerrainMapUtils::makeEditableTexturePath(project.get(), scene->id, terrainEntity, heightRef);
    created.id = created.path;
    created.colorFormat = ColorFormat::RED16;
    created.channels = 1;
    created.width = created.height = 256;
    created.pixels = TerrainMapUtils::makeInitialMapPixels(TerrainMapTarget::HeightMap, 256, 256, true);
    madeIds.push_back(created.path);
    const size_t rawBytes = created.pixels.size();
    auto* create = new TerrainTextureEditCmd(project.get(), scene->id, terrainEntity, heightRef, empty, created);
    EXPECT_LT(create->memoryBytes(), rawBytes / 4) << "a flat map compresses";
    ASSERT_TRUE(history()->addCommandNoMerge(create));
    const std::vector<unsigned char> createdBytes = bytesOf(HEIGHT_MAP);
    EXPECT_EQ(createdBytes, created.pixels);
    ASSERT_TRUE(history()->undo());
    EXPECT_TRUE(terrain().heightMap.empty());
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(bytesOf(HEIGHT_MAP), createdBytes);

    // resize: another file of another size
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project.get(), terrain().heightMap, true);
    TerrainMapSnapshot resized = before;
    resized.path = TerrainMapUtils::makeEditableTexturePath(project.get(), scene->id, terrainEntity, heightRef);
    resized.id = resized.path;
    resized.width = resized.height = 128;
    resized.pixels = TerrainMapUtils::makeInitialMapPixels(TerrainMapTarget::HeightMap, 128, 128, false);
    madeIds.push_back(resized.path);
    const std::vector<unsigned char> resizedBytes = resized.pixels;
    ASSERT_TRUE(history()->addCommandNoMerge(new TerrainTextureEditCmd(project.get(), scene->id, terrainEntity, heightRef, before, std::move(resized))));
    EXPECT_EQ(bytesOf(HEIGHT_MAP), resizedBytes);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(bytesOf(HEIGHT_MAP), createdBytes);
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(bytesOf(HEIGHT_MAP), resizedBytes);
    ASSERT_TRUE(history()->undo());

    // convert to 16-bit: the same heights, each 8-bit level at its 16-bit equal
    giveFileMap(heightRef, 32, ColorFormat::RED, 1, [](size_t i) { return i % 256; });
    const std::vector<unsigned char> eightBit = bytesOf(HEIGHT_MAP);
    TerrainEditWindow window(project.get());
    std::string error;
    ASSERT_TRUE(window.convertHeightMapTo16Bit(scene->id, terrainEntity, error)) << error;
    const std::vector<unsigned char> sixteenBit = bytesOf(HEIGHT_MAP);
    ASSERT_EQ(sixteenBit.size(), eightBit.size() * 2);
    for (size_t i = 0; i < eightBit.size(); i++) {
        const unsigned value = sixteenBit[i * 2] | (static_cast<unsigned>(sixteenBit[i * 2 + 1]) << 8);
        ASSERT_EQ(value, eightBit[i] * 257u) << "texel " << i;
    }
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(bytesOf(HEIGHT_MAP), eightBit);
    EXPECT_NE(TerrainMapUtils::expectedFormat(TerrainMapTarget::HeightMap), terrain().heightMap.getData().getColorFormat());
}
