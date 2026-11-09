// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Terrain World's folder in a project and in a build (T4 R38, R48-R53; AC28's files, AC30's web
// limits, AC32, AC33's cook, AC35's determinism)

#include <gtest/gtest.h>

#include "asset/AssetType.h"
#include "asset/AssetTypes.h"
#include "cook/TerrainWorldCooker.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/AssetDatabase.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;
namespace fs = std::filesystem;

namespace {

    struct ScratchDir {
        fs::path path;
        ScratchDir() {
            std::random_device device;
            path = fs::temp_directory_path() / ("je-terrainworld-" + std::to_string(device()) + "-" +
                                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }
        ~ScratchDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    std::string readScratchFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    WorldManifest smallManifest(int tileQuads = 64) {
        WorldManifest manifest;
        manifest.lattice.tileQuads = tileQuads;
        manifest.lattice.spacing = 1.0f;
        manifest.maps.maxHeight = 80.0f;
        manifest.maps.baseHeight = 1000;
        return manifest;
    }

    std::vector<TileCoord> grid(int low, int high) {
        std::vector<TileCoord> tiles;
        for (int z = low; z <= high; z++) {
            for (int x = low; x <= high; x++) tiles.emplace_back(x, z);
        }
        return tiles;
    }

}

TEST(TerrainWorldAsset, AssetTypeValueIs31) {
    // stored in assetmap.bin: a file format (programme D22)
    EXPECT_EQ(static_cast<int>(AssetType::TerrainWorld), 31);
    EXPECT_STREQ(assetTypeName(AssetType::TerrainWorld), "terrain_world");
    EXPECT_EQ(assetTypeFromName("terrain_world"), AssetType::TerrainWorld);
    EXPECT_EQ(assetTypeFromName("tree_data"), AssetType::TreeData);
    const AssetTypeInfo* info = AssetTypes::find("assets/worlds/Island.terrainworld", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::TerrainWorld);
    EXPECT_EQ(info->identity, AssetIdentity::InFile);
    EXPECT_EQ(AssetTypes::find("assets/worlds/Island.terrainworld/tiles/0_0.jtile", "assets"), nullptr) << "a tile is no asset";
    EXPECT_TRUE(AssetTypes::isFolderAsset("Island.terrainworld"));
    EXPECT_FALSE(AssetTypes::isFolderAsset(".terrainworld"));
    EXPECT_EQ(AssetTypes::identityFileOf("a/Island.terrainworld"), fs::path("a/Island.terrainworld/manifest.yaml"));
}

TEST(TerrainWorldAsset, TerrainWorldIsOneAsset) {
    ScratchDir dir;
    const fs::path project = dir.path / "project";
    fs::create_directories(project / "assets");
    std::ofstream(project / "project.yaml") << "name: worlds\n";
    WorldManifest manifest = smallManifest();
    std::string error;
    ASSERT_TRUE(createWorldFolder(project / "assets" / "Island.terrainworld", manifest, grid(0, 1), error)) << error;
    ASSERT_FALSE(manifest.guid.empty());

    AssetDatabase database;
    database.open(project, "assets");
    // R48: the folder is one asset, its GUID the manifest's, and nothing inside it is tracked
    EXPECT_EQ(database.getAssetGuid("assets/Island.terrainworld").str(), manifest.guid);
    EXPECT_EQ(database.assetCount(), 1u);
    for (const auto& entry : fs::recursive_directory_iterator(project / "assets" / "Island.terrainworld")) {
        EXPECT_FALSE(AssetTypes::isMetaFile(entry.path())) << entry.path();
    }
    // moved, it keeps its GUID: it is in the manifest
    fs::rename(project / "assets" / "Island.terrainworld", project / "assets" / "Moved.terrainworld");
    AssetDatabase reopened;
    reopened.open(project, "assets");
    EXPECT_EQ(reopened.getAssetGuid("assets/Moved.terrainworld").str(), manifest.guid);
}

TEST(TerrainWorldAsset, ManifestRoundTrip) {
    ScratchDir dir;
    WorldManifest manifest = smallManifest(128);
    manifest.guid = "0123456789abcdef0123456789abcdef";
    manifest.maps.blendMaps = 2;
    manifest.maps.blendFill[1][0] = 0.25f;
    manifest.maps.hasHoles = true;
    manifest.maps.densityMaps = 3;
    manifest.layerHash = 0xfeedbeefcafe1234ull;
    std::string error;
    ASSERT_TRUE(createWorldFolder(dir.path / "W.terrainworld", manifest, grid(0, 0), error)) << error;
    WorldManifest back;
    ASSERT_TRUE(readWorldManifest(dir.path / "W.terrainworld", back, error)) << error;
    EXPECT_EQ(back.guid, manifest.guid);
    EXPECT_EQ(back.lattice, manifest.lattice);
    EXPECT_EQ(back.maps.maxHeight, 80.0f);
    EXPECT_EQ(back.maps.baseHeight, 1000);
    EXPECT_EQ(back.maps.blendMaps, 2);
    EXPECT_EQ(back.maps.blendFill[1][0], 0.25f);
    EXPECT_TRUE(back.maps.hasHoles);
    EXPECT_EQ(back.maps.densityMaps, 3);
    EXPECT_EQ(back.layerHash, manifest.layerHash);
    ASSERT_EQ(back.tiles.size(), 1u);
    EXPECT_EQ(back.tiles[0].minHeight, 1000);
    // S03 R23: a newer manifest is refused, so nothing overwrites it
    std::string text = readScratchFile(dir.path / "W.terrainworld" / "manifest.yaml");
    text.replace(text.find("formatVersion: 1"), 16, "formatVersion: 9");
    std::ofstream(dir.path / "W.terrainworld" / "manifest.yaml", std::ios::binary) << text;
    EXPECT_FALSE(readWorldManifest(dir.path / "W.terrainworld", back, error));
    EXPECT_NE(error.find("newer"), std::string::npos);
}

TEST(TerrainWorldAsset, TileListIsFiles) {
    // R48: tiles added on two branches merge with no manifest change: the set is the files present
    ScratchDir dir;
    WorldManifest manifest = smallManifest();
    std::string error;
    const fs::path folder = dir.path / "W.terrainworld";
    ASSERT_TRUE(createWorldFolder(folder, manifest, {TileCoord(0, 0)}, error)) << error;
    const std::string manifestBefore = readScratchFile(folder / "manifest.yaml");
    WorldTile branchA, branchB;
    initTile(branchA, manifest.lattice, TileCoord(5, -3), 2000);
    initTile(branchB, manifest.lattice, TileCoord(-4, 7), 3000);
    ASSERT_TRUE(writeTileFile(folder, branchA, error)) << error;
    ASSERT_TRUE(writeTileFile(folder, branchB, error)) << error;
    EXPECT_EQ(readScratchFile(folder / "manifest.yaml"), manifestBefore);
    WorldManifest back;
    ASSERT_TRUE(readWorldManifest(folder, back, error)) << error;
    ASSERT_EQ(back.tiles.size(), 3u);
    EXPECT_EQ(scanWorldTiles(folder).size(), 3u);
    // a stray file is not a tile
    std::ofstream(folder / "tiles" / "notes.txt") << "x";
    std::ofstream(folder / "tiles" / "1_x.jtile") << "x";
    EXPECT_EQ(scanWorldTiles(folder).size(), 3u);
    ASSERT_TRUE(removeTileFile(folder, TileCoord(5, -3), error)) << error;
    EXPECT_EQ(scanWorldTiles(folder).size(), 2u);
}

TEST(TerrainWorldCook, DeterministicPackAndSeamRepair) {
    ScratchDir dir;
    WorldManifest manifest = smallManifest();
    std::string error;
    const fs::path folder = dir.path / "W.terrainworld";
    ASSERT_TRUE(createWorldFolder(folder, manifest, grid(0, 1), error)) << error;
    // AC33: a hand-corrupted border sample of the higher tile
    {
        std::vector<uint8_t> bytes;
        const std::string raw = readScratchFile(folder / terrainWorldTileName(TileCoord(1, 0)));
        WorldTile tile;
        ASSERT_EQ(decodeTile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()), terrainWorldEditorCodec(), tile),
                  TileError::None);
        tile.setSample(1, 10, 40000);
        ASSERT_TRUE(writeTileFile(folder, tile, error)) << error;
    }
    TerrainWorldCookReport first, second;
    ASSERT_TRUE(TerrainWorldCooker::cook(folder, dir.path / "out1" / "W.terrainworld", false, first, error)) << error;
    ASSERT_TRUE(TerrainWorldCooker::cook(folder, dir.path / "out2" / "W.terrainworld", false, second, error)) << error;
    EXPECT_EQ(first.tiles, 4);
    EXPECT_EQ(first.seamRepairs, 1) << "the lower (x, z) wins, reported once";
    EXPECT_FALSE(first.warnings.empty());
    // same input, same bytes (R53)
    for (const char* name : {"world.bin", "overview.jtile", "tiles/0_0.jtile", "tiles/1_0.jtile"}) {
        EXPECT_EQ(readScratchFile(dir.path / "out1" / "W.terrainworld" / name), readScratchFile(dir.path / "out2" / "W.terrainworld" / name)) << name;
    }
    EXPECT_FALSE(fs::exists(dir.path / "out1" / "W.terrainworld" / "manifest.yaml")) << "the player reads world.bin";
    // the cooked world reads back as the player reads it, its seam closed
    CookedWorldSource cooked((dir.path / "out1" / "W.terrainworld").generic_string());
    WorldManifest shipped;
    ASSERT_TRUE(cooked.manifest(shipped, error)) << error;
    EXPECT_EQ(shipped.tiles.size(), 4u);
    const std::string tileBytes = readScratchFile(dir.path / "out1" / "W.terrainworld" / "tiles" / "1_0.jtile");
    WorldTile repaired;
    ASSERT_EQ(decodeTile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(tileBytes.data()), tileBytes.size()), terrainWorldDecodeCodec(), repaired),
              TileError::None);
    EXPECT_EQ(repaired.sample(1, 10), 1000);
    // the source folder is untouched: the cook repairs a copy
    EXPECT_GT(TerrainWorldCooker::cook(folder, dir.path / "out3" / "W.terrainworld", false, second, error), false);
    EXPECT_EQ(second.seamRepairs, 1);
}

TEST(TerrainWorldCook, WebLimits) {
    ScratchDir dir;
    WorldManifest manifest = smallManifest(512);
    std::string error;
    const fs::path folder = dir.path / "Big.terrainworld";
    ASSERT_TRUE(createWorldFolder(folder, manifest, {TileCoord(0, 0)}, error)) << error;
    TerrainWorldCookReport report;
    EXPECT_FALSE(TerrainWorldCooker::cook(folder, dir.path / "web" / "Big.terrainworld", true, report, error));
    EXPECT_NE(error.find("256"), std::string::npos) << error;
    // desktop takes it
    EXPECT_TRUE(TerrainWorldCooker::cook(folder, dir.path / "desktop" / "Big.terrainworld", false, report, error)) << error;
}
