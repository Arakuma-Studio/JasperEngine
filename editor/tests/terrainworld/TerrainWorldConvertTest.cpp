// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Convert to Terrain World (T4 R33, AC29's TerrainWorldConvert): a Terrain with height, paint, holes,
// a detail layer's density and trees becomes a World at its place, as one undo step. Its lattice lies
// on the drawn surface (sampleTerrainSurface) within a 16-bit quantum, its maps resample to the tiles,
// its trees split into each tile's .jetrees, and the source is deactivated, not deleted. A rotated
// source is refused.

#include "../terrain/TerrainProjectFixture.h"

#include "component/TerrainWorldComponent.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/MeshSystem.h"
#include "terrain/FoliageRules.h"
#include "terrain/TreeStore.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrainworld/TerrainWorldConvert.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/FileUtils.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

using namespace terrainprojecttest;
using namespace jasperengine::terraincore;

namespace {

    class TerrainWorldConvertTest : public TerrainProjectFixture {
    protected:
        // a 65 x 65 heightmap over 128 m (texels about 2 m apart: 1 m spacing, 64-quad tiles, 2 x 2 of them)
        void giveSurface() {
            giveFileMap(TerrainMapRef(TerrainMapTarget::HeightMap), 65, ColorFormat::RED16, 1, [](size_t byte) {
                const size_t texel = byte / 2;
                const int x = static_cast<int>(texel % 65), y = static_cast<int>(texel / 65);
                const uint16_t value = static_cast<uint16_t>(20000 + 9000 * std::sin(x * 0.21) + 7000 * std::cos(y * 0.17) + 50 * ((x * 7 + y * 3) % 11));
                return byte % 2 == 0 ? (value & 0xFF) : (value >> 8);
            });
            giveFileMap(TerrainMapRef(TerrainMapTarget::BlendMap, 0), 32, ColorFormat::RGBA, 4, [](size_t byte) {
                return static_cast<unsigned char>((byte * 37 + (byte / 4) * 11) & 0xFF);
            });
            giveFileMap(TerrainMapRef(TerrainMapTarget::HoleMap), 32, ColorFormat::RED, 1, [](size_t texel) {
                return static_cast<unsigned char>(texel % 32 < 4 && texel / 32 < 4 ? 0 : 255);
            });
            giveFileMap(TerrainMapRef(TerrainMapTarget::DensityMap, 0), 32, ColorFormat::RED, 1, [](size_t texel) {
                return static_cast<unsigned char>((texel * 13) & 0xFF);
            });
        }

        // three trees through the store, as a painted Terrain's are
        void giveTrees() {
            FoliageSystem* foliage = scene->scene->getSystem<FoliageSystem>().get();
            foliage->update(0.0);
            std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(terrainEntity);
            ASSERT_NE(store, nullptr);
            std::vector<TreeRecord> records;
            for (const auto& [x, z] : std::vector<std::pair<float, float>>{{-40.0f, -30.0f}, {10.0f, -50.0f}, {33.0f, 47.0f}}) {
                TreeRecord record{};
                record.u = x / 128.0f + 0.5f;
                record.v = z / 128.0f + 0.5f;
                record.height = 0.4f;
                record.scale = 1.0f;
                record.rotation[3] = 32767;
                record.prototypeId = 5;
                record.heightRatio = 128;
                std::memset(record.color, 255, 4);
                records.push_back(record);
            }
            TreeDelta delta;
            store->apply({}, records, {}, &delta);
            foliage->treesChanged(terrainEntity, delta);
        }

        std::shared_ptr<WorldTile> readTile(const fs::path& folder, TileCoord coord) {
            std::ifstream file(folder / ("tiles/" + std::to_string(coord.x) + "_" + std::to_string(coord.z) + ".jtile"), std::ios::binary);
            std::ostringstream text;
            text << file.rdbuf();
            const std::string bytes = text.str();
            auto tile = std::make_shared<WorldTile>();
            EXPECT_EQ(decodeTile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), terrainWorldEditorCodec(), *tile),
                      TileError::None);
            return tile;
        }
    };

}

TEST_F(TerrainWorldConvertTest, SurfaceMatches) {
    giveSurface();
    giveTrees();
    scene->scene->getComponent<Transform>(terrainEntity).position = Vector3(10.0f, 0.0f, -5.0f);
    terrain().textureDetailTiles = 32.0f;
    const size_t stepsBefore = history()->position();

    const TerrainConvertResult result = convertTerrainNow(project.get(), scene->id, terrainEntity, TerrainConvertOptions());
    ASSERT_NE(result.entity, NULL_ENTITY) << result.error;
    EXPECT_EQ(result.tiles, 4);
    EXPECT_EQ(history()->position(), stepsBefore + 1) << "one undo step";
    Scene* s = scene->scene;
    const Entity world = result.entity;

    // at the same place, the source kept but inactive
    ASSERT_NE(s->findComponent<TerrainWorldComponent>(world), nullptr);
    EXPECT_EQ(s->getComponent<Transform>(world).position, Vector3(10.0f, 0.0f, -5.0f));
    EXPECT_FALSE(s->isEntityActive(terrainEntity));
    ASSERT_TRUE(s->isEntityCreated(terrainEntity));
    // detail tiling keeps its metres; the layers come along, the maps are the tiles' now
    const TerrainComponent& made = s->getComponent<TerrainComponent>(world);
    EXPECT_FLOAT_EQ(made.textureDetailTiles / made.terrainSize, terrain().textureDetailTiles / terrain().terrainSize);
    EXPECT_EQ(made.foliageLayers.size(), terrain().foliageLayers.size());
    EXPECT_TRUE(made.foliageLayers[0].densityMap.empty());
    EXPECT_TRUE(made.blendMaps.empty());

    const fs::path folder = project->resolveAssetPath(FileUtils::pathFromUtf8(result.assetPath));
    WorldManifest manifest;
    std::string error;
    ASSERT_TRUE(readWorldManifest(folder, manifest, error)) << error;
    ASSERT_EQ(manifest.tiles.size(), 4u);
    EXPECT_EQ(manifest.lattice.tileQuads, 64);
    EXPECT_FLOAT_EQ(manifest.lattice.spacing, 1.0f);
    EXPECT_EQ(manifest.maps.blendMaps, 1);
    EXPECT_TRUE(manifest.maps.hasHoles);
    EXPECT_EQ(manifest.maps.densityMaps, 1);
    std::map<uint32_t, std::shared_ptr<WorldTile>> tiles;
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) tiles[TileCoord(x, z).key()] = readTile(folder, TileCoord(x, z));
    }

    // 1,000 lattice samples: the tile's sample is the source's surface there, within one quantum
    const float quantum = terrain().maxHeight / 65535.0f;
    uint32_t state = 12345u;
    const auto next = [&state](int range) {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % static_cast<uint32_t>(range));
    };
    for (int k = 0; k < 1000; k++) {
        const int sx = next(129) - 64, sz = next(129) - 64;
        const TileCoord coord(sx >= 64 ? 0 : (sx < 0 ? -1 : 0), sz >= 64 ? 0 : (sz < 0 ? -1 : 0));
        const WorldTile& tile = *tiles[coord.key()];
        const float worldHeight = heightOfSample(tile.sample(sx - coord.x * 64 + 1, sz - coord.z * 64 + 1), terrain().maxHeight);
        float sourceHeight = 0.0f;
        Vector3 normal;
        MeshSystem::sampleTerrainSurface(terrain(), static_cast<float>(sx), static_cast<float>(sz), sourceHeight, normal);
        ASSERT_NEAR(worldHeight, sourceHeight, quantum) << sx << ", " << sz;
    }

    // paint, holes and density resample to the tiles' lattices
    const WorldTile& corner = *tiles[TileCoord(-1, -1).key()];
    ASSERT_EQ(corner.holes.width, 64);
    EXPECT_EQ(*corner.holes.texel(0, 0), 0) << "the source's hole corner";
    EXPECT_EQ(*corner.holes.texel(40, 40), 255);
    ASSERT_EQ(corner.density.size(), 1u);
    ASSERT_EQ(corner.density[0].width, 64);
    // texel (20, 7) of tile (-1, -1) is centred at (-43.5, -56.5): source texel (5, 1), floor((x + 64) / 128 * 32)
    EXPECT_EQ(*corner.density[0].texel(20, 7), static_cast<uint8_t>((37 * 13) & 0xFF));
    ASSERT_EQ(corner.blend.size(), 1u);
    EXPECT_EQ(corner.blend[0].width, 33);

    // the trees: each in the tile under it, at the same place
    size_t treeFiles = 0, trees = 0;
    for (const WorldTileInfo& info : manifest.tiles) {
        if (!info.trees) continue;
        treeFiles++;
        std::ifstream file(folder / ("tiles/" + std::to_string(info.coord.x) + "_" + std::to_string(info.coord.z) + ".jetrees"), std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        const std::string bytes = text.str();
        TreeStore store;
        ASSERT_TRUE(store.decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), nullptr));
        for (const TreeRecord& record : store.records()) {
            const float x = record.u * 64.0f + static_cast<float>(info.coord.x) * 64.0f;
            const float z = record.v * 64.0f + static_cast<float>(info.coord.z) * 64.0f;
            EXPECT_TRUE((std::abs(x + 40.0f) < 1e-3f && std::abs(z + 30.0f) < 1e-3f) || (std::abs(x - 10.0f) < 1e-3f && std::abs(z + 50.0f) < 1e-3f) ||
                        (std::abs(x - 33.0f) < 1e-3f && std::abs(z - 47.0f) < 1e-3f)) << x << ", " << z;
            trees++;
        }
    }
    EXPECT_EQ(treeFiles, 3u);
    EXPECT_EQ(trees, 3u);

    // undo: the World goes, the Terrain is back as it was; the folder stays on disk (the next
    // conversion picks another name), redo brings the World back over it
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(s->isEntityCreated(world));
    EXPECT_TRUE(s->isEntityActive(terrainEntity));
    EXPECT_FALSE(terrain().heightMap.empty());
    EXPECT_EQ(terrain().blendMaps.size(), 1u);
    EXPECT_TRUE(fs::exists(folder / "manifest.yaml"));
    EXPECT_NE(freeTerrainWorldAssetPath(project.get(), "Terrain World"), result.assetPath);
    ASSERT_TRUE(history()->redo());
    EXPECT_FALSE(s->isEntityActive(terrainEntity));
}

TEST_F(TerrainWorldConvertTest, RotatedOrScaledSourceIsRefused) {
    giveHeight16(33);
    Transform& transform = scene->scene->getComponent<Transform>(terrainEntity);
    transform.rotation = Quaternion(30.0f, Vector3(0.0f, 1.0f, 0.0f));
    transform.worldRotation = transform.rotation;
    const size_t stepsBefore = history()->position();
    const TerrainConvertResult rotated = convertTerrainNow(project.get(), scene->id, terrainEntity, TerrainConvertOptions());
    EXPECT_EQ(rotated.entity, NULL_ENTITY);
    EXPECT_NE(rotated.error.find("does not rotate or scale"), std::string::npos) << rotated.error;

    transform.rotation = Quaternion();
    transform.worldRotation = Quaternion();
    transform.scale = Vector3(2.0f, 1.0f, 2.0f);
    transform.worldScale = transform.scale;
    const TerrainConvertResult scaled = convertTerrainNow(project.get(), scene->id, terrainEntity, TerrainConvertOptions());
    EXPECT_EQ(scaled.entity, NULL_ENTITY);
    EXPECT_EQ(history()->position(), stepsBefore) << "nothing was changed";
    EXPECT_TRUE(scene->scene->isEntityActive(terrainEntity));
}
