// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// R46, R47, AC8: applying an import plan is one undo step, and a linked Grid re-imports in place,
// matching layers by name and keeping their GameObjects and extra components.

#include "tilemap/import/TilemapImportApply.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "component/GridComponent.h"
#include "component/ScriptComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "asset/ImportedAssets.h"
#include "pixelart/PixelImage.h"
#include "support/EditorTestSupport.h"
#include "reload/HotReloadService.h"
#include "support/TestTempDir.h"
#include "tilemap/Tileset.h"
#include "tilemap/TilesetPool.h"

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
using namespace jasperengine::editor::tilemapimport;

namespace {

    // a 2x2 orthogonal map; gids 1..2 of a 2-tile tileset
    std::string mapText(const std::string& layersJson) {
        return R"({"type":"map","orientation":"orthogonal","width":2,"height":2,"tilewidth":16,"tileheight":16,"infinite":false,
            "tilesets":[{"firstgid":1,"name":"terrain","tilewidth":16,"tileheight":16,"tilecount":2,"columns":2,"image":"tiles.png","imagewidth":32,"imageheight":16}],
            "layers":[)" + layersJson + "]}";
    }

    std::string layerJson(const std::string& name, const std::string& data) {
        return R"({"type":"tilelayer","name":")" + name + R"(","width":2,"height":2,"data":)" + data + "}";
    }

    class TilemapImportApplyTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"tilemap-import-apply"};
        Project* project = nullptr;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;
        fs::path mapFile;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
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
            fs::create_directories(project->getAssetsPath() / "maps");
            mapFile = project->getAssetsPath() / "maps" / "level.tmj";
            writeMap(mapText(layerJson("Ground", "[1,0,0,2]") + "," + layerJson("Extra", "[2,2,0,0]")));
        }
        void TearDown() override {
            if (project) jasperengine::editor::test::closeProject(*project);
        }

        void writeMap(const std::string& text) {
            std::ofstream out(mapFile, std::ios::binary | std::ios::trunc);
            out << text;
        }

        ImportPlan loadTheMap() {
            ImportPlan plan;
            std::vector<ImportProblem> problems;
            EXPECT_TRUE(loadPlan(project, mapFile, plan, problems)) << (problems.empty() ? std::string() : problems[0].message);
            return plan;
        }

        ImportTarget target(bool linked = false) const {
            ImportTarget t;
            t.project = project;
            t.sceneId = sceneId;
            t.mapFile = mapFile;
            t.linked = linked;
            return t;
        }

        Entity layerNamed(Entity grid, const std::string& name) const {
            for (const Entity entity : scene->entities) {
                const Transform* transform = scene->scene->findComponent<Transform>(entity);
                if (transform && transform->parent == grid && scene->scene->getEntityName(entity) == name) return entity;
            }
            return NULL_ENTITY;
        }

        const TileCells* cellsOf(Entity layer) const {
            const TilemapLayerComponent* component = scene->scene->findComponent<TilemapLayerComponent>(layer);
            return component ? &component->cells.read() : nullptr;
        }
    };

}

TEST_F(TilemapImportApplyTest, PreviewWritesNothingAndApplyIsOneUndoStep) {
    const ImportPlan plan = loadTheMap();
    const ImportReport report = previewReport(plan, target());
    EXPECT_EQ(report.counts.layers, 2);
    ASSERT_EQ(report.tilesets.size(), 1u);
    EXPECT_EQ(report.tilesets[0].file, "maps/terrain.tileset");
    const size_t before = CommandHandle::get(sceneId)->position();
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before);
    EXPECT_FALSE(fs::exists(mapFile.parent_path() / "terrain.tileset"));

    const ImportResult result = apply(plan, target());
    ASSERT_TRUE(result.ok) << result.problem.message;
    ASSERT_EQ(result.grids.size(), 1u);
    ASSERT_EQ(result.layers.size(), 2u);
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before + 1) << "one undo step";

    const Entity grid = result.grids.front();
    const GridComponent* gridComponent = scene->scene->findComponent<GridComponent>(grid);
    ASSERT_NE(gridComponent, nullptr);
    EXPECT_EQ(gridComponent->layout, TileGridLayout::Rectangle);
    EXPECT_FALSE(gridComponent->linked);
    EXPECT_EQ(scene->scene->getEntityName(grid), "level");

    const Entity ground = layerNamed(grid, "Ground");
    const Entity extra = layerNamed(grid, "Extra");
    ASSERT_NE(ground, NULL_ENTITY);
    ASSERT_NE(extra, NULL_ENTITY);
    EXPECT_EQ(scene->scene->getComponent<TilemapLayerComponent>(ground).tileset.path, "maps/terrain.tileset");
    // Tiled's (0,0) is our top row (y 1), its (1,1) our bottom row (y 0); y up
    const TileCells* cells = cellsOf(ground);
    ASSERT_NE(cells, nullptr);
    EXPECT_EQ(cells->getTile(CellCoord{0, 1}), 0);
    EXPECT_EQ(cells->getTile(CellCoord{1, 0}), 1);
    EXPECT_EQ(cells->getTile(CellCoord{0, 0}), NO_TILE);

    ASSERT_TRUE(fs::exists(mapFile.parent_path() / "terrain.tileset"));
    std::ifstream in(mapFile.parent_path() / "terrain.tileset");
    std::stringstream text;
    text << in.rdbuf();
    TilesetData tileset;
    std::vector<TilesetProblem> problems;
    ASSERT_TRUE(tileset::parse(text.str(), tileset, problems));
    EXPECT_EQ(tileset.sources[0].asset.path, "maps/tiles.png");

    // one undo takes everything back: GameObjects and the tileset file
    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before);
    EXPECT_EQ(scene->scene->findComponent<GridComponent>(grid), nullptr);
    EXPECT_FALSE(fs::exists(mapFile.parent_path() / "terrain.tileset"));

    ASSERT_TRUE(CommandHandle::get(sceneId)->redo());
    EXPECT_NE(scene->scene->findComponent<GridComponent>(grid), nullptr);
    const TileCells* redone = cellsOf(layerNamed(grid, "Ground"));
    ASSERT_NE(redone, nullptr);
    EXPECT_EQ(redone->getTile(CellCoord{1, 0}), 1);
    EXPECT_TRUE(fs::exists(mapFile.parent_path() / "terrain.tileset"));
}

TEST_F(TilemapImportApplyTest, ExistingFilesAreNotOverwrittenByAnImport) {
    { std::ofstream out(mapFile.parent_path() / "terrain.tileset"); out << "{}"; }
    const ImportResult result = apply(loadTheMap(), target());
    ASSERT_TRUE(result.ok) << result.problem.message;
    ASSERT_EQ(result.tilesetFiles.size(), 1u);
    EXPECT_EQ(result.tilesetFiles[0], "maps/terrain_2.tileset");
    std::ifstream in(mapFile.parent_path() / "terrain.tileset");
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), "{}");
}

TEST_F(TilemapImportApplyTest, LinkedReimportMatchesLayersByNameAndKeepsUserComponents) {
    const ImportResult imported = apply(loadTheMap(), target(true));
    ASSERT_TRUE(imported.ok) << imported.problem.message;
    const Entity grid = imported.grids.front();
    const GridComponent* gridNow = scene->scene->findComponent<GridComponent>(grid);
    ASSERT_NE(gridNow, nullptr);
    EXPECT_TRUE(gridNow->linked);
    EXPECT_EQ(gridNow->importSource.path, "maps/level.tmj");

    const Entity ground = layerNamed(grid, "Ground");
    const Entity extra = layerNamed(grid, "Extra");
    ASSERT_NE(ground, NULL_ENTITY);
    ASSERT_NE(extra, NULL_ENTITY);
    scene->scene->addComponent<ScriptComponent>(ground, ScriptComponent{});

    // an unchanged map adds no undo step
    size_t before = CommandHandle::get(sceneId)->position();
    ImportResult same = reimportLinked(project, sceneId, grid);
    ASSERT_TRUE(same.ok) << same.problem.message;
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before);

    // Ground's cells change, Extra leaves the file, New joins it
    writeMap(mapText(layerJson("Ground", "[0,0,2,2]") + "," + layerJson("New", "[1,0,0,0]")));
    before = CommandHandle::get(sceneId)->position();
    linkedSourcesChanged(*project, {mapFile});
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before + 1) << "one undo step";

    ASSERT_NE(scene->scene->findComponent<TilemapLayerComponent>(ground), nullptr);
    EXPECT_NE(scene->scene->findComponent<ScriptComponent>(ground), nullptr) << "the user component stays";
    const TileCells* groundCells = cellsOf(ground);
    EXPECT_EQ(groundCells->getTile(CellCoord{0, 1}), NO_TILE);
    EXPECT_EQ(groundCells->getTile(CellCoord{0, 0}), 1);
    EXPECT_EQ(groundCells->getTile(CellCoord{1, 0}), 1);
    EXPECT_EQ(layerNamed(grid, "Extra"), NULL_ENTITY);
    const Entity added = layerNamed(grid, "New");
    ASSERT_NE(added, NULL_ENTITY);
    EXPECT_EQ(cellsOf(added)->getTile(CellCoord{0, 1}), 0);

    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    EXPECT_NE(layerNamed(grid, "Extra"), NULL_ENTITY);
    EXPECT_EQ(layerNamed(grid, "New"), NULL_ENTITY);
    EXPECT_EQ(cellsOf(ground)->getTile(CellCoord{0, 1}), 0);
    EXPECT_NE(scene->scene->findComponent<ScriptComponent>(ground), nullptr);
}

// R47 through S05's funnel: the registration in HotReloadService::apply, not a direct call. The
// base EditorHost these tests run on makes the service's deferred main-thread task run inline.
TEST_F(TilemapImportApplyTest, HotReloadServiceReimportsALinkedMapChangedOnDisk) {
    const ImportResult imported = apply(loadTheMap(), target(true));
    ASSERT_TRUE(imported.ok) << imported.problem.message;
    const Entity grid = imported.grids.front();
    const Entity ground = layerNamed(grid, "Ground");
    ASSERT_NE(ground, NULL_ENTITY);
    scene->scene->addComponent<ScriptComponent>(ground, ScriptComponent{});

    struct AttachedService {
        HotReloadService& service = HotReloadService::get();
        explicit AttachedService(Project& owner) { service.attach(owner); }
        ~AttachedService() { service.detach(); }
    } attached(*project);

    const auto changeOf = [](const fs::path& path, FileChangeKind kind) {
        FileChange change;
        change.kind = kind;
        change.path = path;
        ChangeSet set;
        set.changes.push_back(change);
        return set;
    };

    // another program saves the map
    writeMap(mapText(layerJson("Ground", "[0,0,2,2]") + "," + layerJson("New", "[1,0,0,0]")));
    const size_t before = CommandHandle::get(sceneId)->position();
    attached.service.apply(changeOf(mapFile, FileChangeKind::Modified), ApplyOptions());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before + 1) << "one undo step";
    EXPECT_EQ(cellsOf(ground)->getTile(CellCoord{0, 1}), NO_TILE);
    EXPECT_EQ(cellsOf(ground)->getTile(CellCoord{0, 0}), 1);
    EXPECT_NE(scene->scene->findComponent<ScriptComponent>(ground), nullptr) << "the user component stays";
    EXPECT_EQ(layerNamed(grid, "Extra"), NULL_ENTITY);
    EXPECT_NE(layerNamed(grid, "New"), NULL_ENTITY);

    // a file nothing links to, and the map's own removal, re-import nothing
    const size_t afterReimport = CommandHandle::get(sceneId)->position();
    const fs::path other = project->getAssetsPath() / "maps" / "other.tmj";
    { std::ofstream out(other, std::ios::binary); out << mapText(layerJson("Ground", "[2,2,2,2]")); }
    attached.service.apply(changeOf(other, FileChangeKind::Modified), ApplyOptions());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), afterReimport);
    fs::remove(mapFile);
    attached.service.apply(changeOf(mapFile, FileChangeKind::Removed), ApplyOptions());
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), afterReimport);
    EXPECT_EQ(cellsOf(ground)->getTile(CellCoord{0, 0}), 1) << "a deleted source keeps the layers";
}

TEST_F(TilemapImportApplyTest, ABrokenSourceKeepsThePreviousVersion) {
    const ImportResult imported = apply(loadTheMap(), target(true));
    ASSERT_TRUE(imported.ok) << imported.problem.message;
    const Entity grid = imported.grids.front();
    const Entity ground = layerNamed(grid, "Ground");
    writeMap("{not json");
    const size_t before = CommandHandle::get(sceneId)->position();
    const ImportResult result = reimportLinked(project, sceneId, grid);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.problem.kind, "parse");
    EXPECT_EQ(CommandHandle::get(sceneId)->position(), before);
    EXPECT_EQ(cellsOf(ground)->getTile(CellCoord{0, 1}), 0);
}

TEST_F(TilemapImportApplyTest, PrefabTargetWritesAPrefabAndLeavesNoGameObjects) {
    ImportTarget t = target();
    t.prefab = project->getAssetsPath() / "prefabs" / "level.prefab";
    const size_t entitiesBefore = scene->entities.size();
    const ImportResult result = apply(loadTheMap(), t);
    ASSERT_TRUE(result.ok) << result.problem.message;
    EXPECT_FALSE(result.prefab.empty());
    EXPECT_TRUE(fs::exists(t.prefab));
    EXPECT_EQ(scene->entities.size(), entitiesBefore);
    EXPECT_TRUE(result.grids.empty());
    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    EXPECT_FALSE(fs::exists(t.prefab));
}

TEST(TilemapImportAsepriteStrip, AReimportedStripReachesTheTileset) {
    jasperengine::test::TestTempDir dir{"tilemap-import-strip"};
    const fs::path first = dir.path() / "strip_first.png";
    const fs::path second = dir.path() / "strip_second.png";
    jasperengine::editor::pixelart::PixelImage image(16, 32);
    ASSERT_TRUE(image.savePng(first));
    ASSERT_TRUE(image.savePng(second));

    auto answer = std::make_shared<std::string>(first.string());
    ImportedAssets::setProvider([answer](const std::string& reference, std::string& out) {
        if (reference != "art/world.aseprite#tileset#0") return false;
        out = *answer;
        return true;
    });

    auto data = std::make_shared<TilesetData>();
    data->tileWidth = 16;
    data->tileHeight = 16;
    TilesetSource source;
    source.kind = TilesetSourceKind::Aseprite;
    source.asset = "art/world.aseprite";
    source.asepriteTileset = 0;
    data->sources.push_back(source);
    TileDef tile;
    tile.atX = 0;
    data->tiles.push_back(tile);
    data->nextId = 1;

    TilesetPool::set("maps/strip.tileset", data);
    std::shared_ptr<const TilesetRuntime> before = TilesetPool::get("maps/strip.tileset");
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->pages.size(), 1u);
    EXPECT_EQ(before->pages[0], first.string());

    *answer = second.string();
    TilesetPool::asepriteSourceChanged("art/world.aseprite");
    std::shared_ptr<const TilesetRuntime> after = TilesetPool::get("maps/strip.tileset");
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->pages.size(), 1u);
    EXPECT_EQ(after->pages[0], second.string());
    EXPECT_NE(after->generation, before->generation);

    ImportedAssets::setProvider(nullptr);
    TilesetPool::invalidate("maps/strip.tileset");
}
