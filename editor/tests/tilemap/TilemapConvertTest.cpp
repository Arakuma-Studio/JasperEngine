// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// "Convert to Grid Tilemap" (docs/specs/S50-grid-tilemap.md R50, AC9) on a real project: an aligned
// legacy map keeps its rect ids and names, a misaligned one is refused listing its tiles, and one
// undo brings the legacy component, the Mesh and the file set back exactly.

#include "tilemap/TilemapConvert.h"

#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "component/GridComponent.h"
#include "component/MeshComponent.h"
#include "component/ScriptComponent.h"
#include "component/TilemapComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "pixelart/PixelImage.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"
#include "tilemap/Tileset.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

namespace {

    class TilemapConvertTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"tilemap-convert"};
        Project* project = nullptr;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            // useProjectPaths registers the project for release when the binary ends, so it must
            // outlive the test
            static std::vector<std::unique_ptr<Project>> keptAlive;
            keptAlive.push_back(std::make_unique<Project>());
            project = keptAlive.back().get();
            project->setSkipProjectLock(true);
            jasperengine::editor::test::useProjectPaths(project);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            jasperengine::editor::test::useProjectPaths(project);
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            scene->isModified = false;

            // a 32x16 atlas: the texture is on disk because a normalized rect needs its size
            jasperengine::editor::pixelart::PixelImage atlas(32, 16);
            atlas.fill({10, 20, 30, 255});
            ASSERT_TRUE(atlas.savePng(project->getAssetsPath() / "tiles.png"));

            entity = scene->scene->createEntity();
            sceneProject().entities.push_back(entity);
            scene->scene->setEntityName(entity, "Legacy Map");
            scene->scene->addComponent<Transform>(entity, Transform{});
            scene->scene->addComponent<MeshComponent>(entity, MeshComponent{});
            scene->scene->addComponent<TilemapComponent>(entity, TilemapComponent{});
            MeshComponent& mesh = scene->scene->getComponent<MeshComponent>(entity);
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].material.baseColorTexture.setPath("tiles.png");
            // the texture's GUID, as a saved scene has it: the conversion's own refresh would stamp it
            project->refreshAssetDatabase(true);
        }
        void TearDown() override {
            if (project) jasperengine::editor::test::closeProject(*project);
        }

        SceneProject& sceneProject() { return *scene; }

        TilemapComponent& tilemap() { return scene->scene->getComponent<TilemapComponent>(entity); }

        void addRect(const std::string& name, float x, float y, float w, float h, int submesh = 0) {
            TilemapComponent& map = tilemap();
            TileRectData rect;
            rect.name = name;
            rect.submeshId = submesh;
            rect.rect = Rect(x, y, w, h);
            map.tilesRect[map.numTilesRect] = rect;
            map.numTilesRect++;
        }

        void addTile(int rectId, float x, float y, float w = 16.0f, float h = 16.0f, const std::string& name = "") {
            TilemapComponent& map = tilemap();
            TileData tile;
            tile.name = name;
            tile.rectId = rectId;
            tile.position = Vector2(x, y);
            tile.width = w;
            tile.height = h;
            map.tiles[map.numTiles] = tile;
            map.numTiles++;
        }

        std::string encoded() {
            return YAML::Dump(Stream::encodeEntity(entity, scene->scene, nullptr, nullptr));
        }
    };

}

TEST_F(TilemapConvertTest, AlignedMapKeepsRectIdsAndNames) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    addRect("stone", 16.0f, 0.0f, 16.0f, 16.0f);
    addTile(1, 0.0f, 0.0f);
    addTile(0, 16.0f, 16.0f);
    const std::string transformBefore = scene->scene->getEntityName(entity);

    const ConvertResult result = convert(project, sceneId, entity);
    ASSERT_TRUE(result.ok) << result.problem.message;

    EXPECT_FALSE(scene->scene->findComponent<TilemapComponent>(entity));
    const GridComponent* grid = scene->scene->findComponent<GridComponent>(entity);
    ASSERT_TRUE(grid);
    EXPECT_FLOAT_EQ(grid->cellSize.x, 16.0f);
    TilemapLayerComponent& layer = scene->scene->getComponent<TilemapLayerComponent>(entity);
    EXPECT_EQ(layer.tileset.path, "tiles.tileset");
    EXPECT_EQ(layer.cells.read().getTile(CellCoord{0, 0}), 1) << "the cell's id is the rect index";
    EXPECT_EQ(layer.cells.read().getTile(CellCoord{1, 1}), 0);
    EXPECT_EQ(scene->scene->getEntityName(entity), transformBefore) << "the same GameObject";

    std::ifstream file(project->getAssetsPath() / "tiles.tileset");
    ASSERT_TRUE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    TilesetData written;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(text.str(), written, problems));
    ASSERT_EQ(written.tiles.size(), 2u);
    EXPECT_EQ(written.tiles[0].id, 0);
    EXPECT_EQ(written.tiles[0].name, "grass");
    EXPECT_EQ(written.tiles[1].id, 1);
    EXPECT_EQ(written.tiles[1].name, "stone");
    EXPECT_EQ(written.tiles[1].atX, 1);
    EXPECT_EQ(written.nextId, 2);
}

TEST_F(TilemapConvertTest, MisalignedMapIsRefusedListingOffenders) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    addTile(0, 0.0f, 0.0f, 16.0f, 16.0f, "ok");
    addTile(0, 8.0f, 0.0f, 16.0f, 16.0f, "off");
    const std::string before = encoded();

    const ConvertResult result = convert(project, sceneId, entity);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.problem.kind, "not_grid_aligned");
    EXPECT_FALSE(result.problem.fix.empty());
    ASSERT_EQ(result.check.offenders.size(), 1u);
    EXPECT_EQ(result.check.offenders[0].tile, 1);
    EXPECT_EQ(result.check.offenders[0].name, "off");
    EXPECT_NE(result.check.offenders[0].reason.find("grid"), std::string::npos);
    EXPECT_EQ(encoded(), before) << "a refusal changes nothing";
    EXPECT_FALSE(fs::exists(project->getAssetsPath() / "tiles.tileset"));
}

TEST_F(TilemapConvertTest, AtMostTenOffendersAreListed) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    for (int i = 0; i < 12; i++) addTile(0, 8.0f + 16.0f * (float)i, 0.0f);
    const ConvertResult result = convert(project, sceneId, entity);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.check.offenders.size(), 10u);
    EXPECT_EQ(result.check.offenderCount, 12u);
}

TEST_F(TilemapConvertTest, TilesOfDifferentSizesAreRefused) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    addTile(0, 0.0f, 0.0f);
    addTile(0, 32.0f, 0.0f, 32.0f, 32.0f);
    EXPECT_EQ(convert(project, sceneId, entity).problem.kind, "not_grid_aligned");
}

TEST_F(TilemapConvertTest, NormalizedRectsUseTheTextureSize) {
    addRect("grass", 0.0f, 0.0f, 0.5f, 1.0f);
    addRect("stone", 0.5f, 0.0f, 0.5f, 1.0f);
    addTile(1, 0.0f, 0.0f);
    const ConvertResult result = convert(project, sceneId, entity);
    ASSERT_TRUE(result.ok) << result.problem.message << (result.check.offenders.empty() ? "" : " / " + result.check.offenders[0].reason);
    TilemapLayerComponent& layer = scene->scene->getComponent<TilemapLayerComponent>(entity);
    EXPECT_EQ(layer.cells.read().getTile(CellCoord{0, 0}), 1);
}

TEST_F(TilemapConvertTest, RectsOfTwoTexturesGiveTwoSources) {
    jasperengine::editor::pixelart::PixelImage other(16, 16);
    other.fill({1, 2, 3, 255});
    ASSERT_TRUE(other.savePng(project->getAssetsPath() / "other.png"));
    MeshComponent& mesh = scene->scene->getComponent<MeshComponent>(entity);
    mesh.numSubmeshes = 2;
    mesh.submeshes[1].material.baseColorTexture.setPath("other.png");
    addRect("a", 0.0f, 0.0f, 16.0f, 16.0f, 0);
    addRect("b", 0.0f, 0.0f, 16.0f, 16.0f, 1);
    addTile(0, 0.0f, 0.0f);
    addTile(1, 16.0f, 0.0f);
    const ConvertResult result = convert(project, sceneId, entity);
    ASSERT_TRUE(result.ok) << result.problem.message;
    ASSERT_EQ(result.check.sources.size(), 2u);
    EXPECT_EQ(result.check.sources[0].texture, "tiles.png");
    EXPECT_EQ(result.check.sources[1].texture, "other.png");
    EXPECT_EQ(result.check.rects[0].source, 0);
    EXPECT_EQ(result.check.rects[1].source, 1);
}

TEST_F(TilemapConvertTest, FlipYAndScaleFactorWarnAndScriptsAreNamed) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    addTile(0, 0.0f, 0.0f);
    tilemap().automaticFlipY = false;
    tilemap().flipY = true;
    tilemap().textureScaleFactor = 0.5f;
    {
        std::ofstream script(project->getAssetsPath() / "uses.lua");
        script << "local map = Tilemap(scene, entity)\n";
    }
    {
        std::ofstream script(project->getAssetsPath() / "clean.lua");
        script << "local layer = TilemapLayer(scene, entity)\n";
    }
    ScriptComponent scripts;
    ScriptEntry uses;
    uses.type = ScriptType::LUA;
    uses.path = AssetRef("uses.lua");
    ScriptEntry clean;
    clean.type = ScriptType::LUA;
    clean.path = AssetRef("clean.lua");
    scripts.scripts = {uses, clean};
    scene->scene->addComponent<ScriptComponent>(entity, scripts);

    const ConvertResult result = convert(project, sceneId, entity);
    ASSERT_TRUE(result.ok) << result.problem.message;
    ASSERT_EQ(result.scriptsUsingTilemap, (std::vector<std::string>{"uses.lua"}));
    std::string warnings;
    for (const std::string& warning : result.warnings) warnings += warning + "\n";
    EXPECT_NE(warnings.find("Flip Y"), std::string::npos) << warnings;
    EXPECT_NE(warnings.find("Texture Scale Factor"), std::string::npos) << warnings;
}

TEST_F(TilemapConvertTest, OneUndoRestoresTheLegacyComponentExactly) {
    addRect("grass", 0.0f, 0.0f, 16.0f, 16.0f);
    addRect("stone", 16.0f, 0.0f, 16.0f, 16.0f);
    addTile(1, 0.0f, 0.0f);
    addTile(0, 16.0f, 0.0f);
    const std::string before = encoded();
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t position = history->position();

    const ConvertResult result = convert(project, sceneId, entity);
    ASSERT_TRUE(result.ok) << result.problem.message;
    EXPECT_EQ(history->position(), position + 1) << "one undo step";
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "tiles.tileset"));

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(encoded(), before) << "the legacy component, the Mesh and the entity as before";
    EXPECT_EQ(scene->scene->findComponent<TilemapLayerComponent>(entity), nullptr);
    ASSERT_TRUE(scene->scene->findComponent<TilemapComponent>(entity));
    EXPECT_EQ(tilemap().numTiles, 2u);
    EXPECT_EQ(tilemap().tilesRect[1].name, "stone");
    EXPECT_FALSE(fs::exists(project->getAssetsPath() / "tiles.tileset")) << "the created file goes with the undo";

    ASSERT_TRUE(history->redo());
    EXPECT_TRUE(scene->scene->findComponent<TilemapLayerComponent>(entity));
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "tiles.tileset"));
    EXPECT_EQ(scene->scene->getComponent<TilemapLayerComponent>(entity).cells.read().getTile(CellCoord{1, 0}), 0);
}

TEST_F(TilemapConvertTest, ANonExistentEntityIsATypedError) {
    const ConvertResult result = convert(project, sceneId, NULL_ENTITY);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.problem.kind, "unknown_layer");
}
