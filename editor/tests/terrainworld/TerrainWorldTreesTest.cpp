// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5's tree tools on a Terrain World (T4 R27): the Paint Trees brush routes each tree to the tile under
// it, in the tile's frame, across borders, and makes a tile's tree file when it has none; the stroke is
// one undo step whose undo and redo give every tile file back byte for byte; Mass Place spreads over
// the tiles. Tiles out of memory are read and written there and then; resident ones through their
// foliage region.

#include "../terrain/TerrainProjectFixture.h"

#include "EditorHost.h"
#include "component/TerrainWorldComponent.h"
#include "foliage/FoliageOpRunner.h"
#include "jobs/JobSystem.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/FoliageOps.h"
#include "terrain/FoliageRules.h"
#include "terrain/TreeStore.h"
#include "terrainworld/TerrainWorldEditor.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "terrainworld/TerrainWorldTrees.h"
#include "util/FileUtils.h"
#include "window/TerrainFoliagePanel.h"

#include "zstd.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace terrainprojecttest;
using namespace jasperengine::terraincore;

namespace {

    constexpr float TREES_TILE = 64.0f;

    // The App's side of enqueueMainThreadTask: tasks wait for the next editor frame
    struct TreesLoopHost : EditorHost {
        std::mutex mutex;
        std::vector<std::function<void()>> tasks;

        void enqueueMainThreadTask(std::function<void()> task) override {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }
        void processMainThreadTasks() override {
            std::vector<std::function<void()>> now;
            {
                std::lock_guard<std::mutex> lock(mutex);
                now.swap(tasks);
            }
            for (std::function<void()>& task : now) task();
        }
    };

    std::map<std::string, std::string> treesFolderFiles(const fs::path& folder) {
        std::map<std::string, std::string> out;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            std::ifstream file(it->path(), std::ios::binary);
            std::ostringstream bytes;
            bytes << file.rdbuf();
            out[FileUtils::pathToUtf8(it->path().lexically_relative(folder))] = bytes.str();
        }
        return out;
    }

    bool treesDecompress(const void* input, size_t inputBytes, void* output, size_t outputBytes) {
        const size_t written = ZSTD_decompress(output, outputBytes, input, inputBytes);
        return !ZSTD_isError(written) && written == outputBytes;
    }

    // A tile's tree file as a store; empty with no file
    TreeStore treesOfFile(const std::map<std::string, std::string>& files, TileCoord tile) {
        TreeStore store;
        const auto found = files.find(jasperengine::terrainWorldTreeName(tile));
        if (found != files.end()) {
            std::string error;
            EXPECT_TRUE(store.decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(found->second.data()), found->second.size()), &error,
                                     treesDecompress)) << error;
        }
        return store;
    }

    class TerrainWorldTreesTest : public TerrainProjectFixture {
    protected:
        TreesLoopHost host;
        Entity world = NULL_ENTITY;
        fs::path folder;
        std::unique_ptr<TerrainFoliagePanel> panel;

        void SetUp() override {
            TerrainProjectFixture::SetUp();
            setEditorHost(&host);
            registerFoliageOps();
            installTerrainWorldHost(project.get());
            TerrainWorldSettings settings;
            settings.name = "Grove";
            settings.tileQuads = 64;
            settings.maxHeight = 32.0f;
            settings.baseHeight = 4.0f;
            std::string error;
            // tiles (-1, -1) to (0, 0), 64 m each
            world = createTerrainWorld(project.get(), scene->id, NULL_ENTITY, settings, error);
            ASSERT_NE(world, NULL_ENTITY) << error;
            folder = project->resolveAssetPath(FileUtils::pathFromUtf8(scene->scene->getComponent<TerrainWorldComponent>(world).worldData.path));
            // the data path only: a prototype with no mesh loads nothing
            TerrainTreePrototype prototype;
            prototype.id = 7;
            scene->scene->getComponent<TerrainComponent>(world).treePrototypes.push_back(prototype);
            ASSERT_NE(runtime(), nullptr);
            panel = std::make_unique<TerrainFoliagePanel>(project.get(), nullptr);
        }

        void TearDown() override {
            panel.reset();
            TerrainWorldSystem::setHost(TerrainWorldSystem::Host());
            setEditorHost(nullptr);
            TerrainProjectFixture::TearDown();
        }

        TerrainWorldRuntime* runtime() {
            std::string error;
            TerrainWorldRuntime* made = terrainWorldRuntimeOf(scene->scene, world, error);
            EXPECT_NE(made, nullptr) << error;
            return made;
        }

        FoliageSystem* foliage() { return scene->scene->getSystem<FoliageSystem>().get(); }

        // every tile in memory with its foliage region, as the Scene view's camera over the World keeps them
        bool makeResident() {
            TerrainWorldRuntime* live = runtime();
            live->tiles().budgets().gpu = false;
            live->addFocus(0.0f, 0.0f);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (std::chrono::steady_clock::now() < deadline) {
                if (jobs::hasSystem() && jobs::system().isSingleThreaded()) jobs::system().pump(4.0);
                scene->scene->getSystem<TerrainWorldSystem>()->pump(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
                foliage()->update(0.016);
                if (foliage()->getRegionCount(world) == 4) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        std::vector<Vector3> points(std::initializer_list<std::pair<float, float>> xz) {
            std::vector<Vector3> out;
            for (const auto& [x, z] : xz) out.push_back(Vector3(x, 0.0f, z));
            return out;
        }

        // every record of the tile within its own square, as its region draws it
        void expectInsideTiles(const std::map<std::string, std::string>& files, size_t& total) {
            total = 0;
            for (int z = -1; z <= 0; z++) {
                for (int x = -1; x <= 0; x++) {
                    const TreeStore store = treesOfFile(files, TileCoord(x, z));
                    for (const TreeRecord& record : store.records()) {
                        EXPECT_GE(foliageTreeLocalX(record, TREES_TILE), -32.0f);
                        EXPECT_LE(foliageTreeLocalX(record, TREES_TILE), 32.0f);
                        EXPECT_GE(foliageTreeLocalZ(record, TREES_TILE), -32.0f);
                        EXPECT_LE(foliageTreeLocalZ(record, TREES_TILE), 32.0f);
                        EXPECT_EQ(record.prototypeId, 7);
                    }
                    total += store.size();
                }
            }
        }
    };

}

TEST_F(TerrainWorldTreesTest, StrokeAcrossBorderWritesBothTilesAndUndoes) {
    // a stroke along z = 16, over the border x = 0 between tiles (-1, 0) and (0, 0); neither has trees yet
    const std::map<std::string, std::string> before = treesFolderFiles(folder);
    ASSERT_FALSE(before.count("tiles/-1_0.jetrees") || before.count("tiles/0_0.jetrees"));
    const size_t steps = history()->position();

    int changed = 0;
    std::string error;
    ASSERT_TRUE(panel->scriptedStroke(scene, world, points({{-12, 16}, {-8, 16}, {-4, 16}, {0, 16}, {4, 16}, {8, 16}, {12, 16}}), 8.0f, false, 0, 0.5f,
                                      changed, error)) << error;
    ASSERT_GT(changed, 0);
    EXPECT_EQ(history()->position(), steps + 1) << "one undo step for the stroke";

    const std::map<std::string, std::string> after = treesFolderFiles(folder);
    ASSERT_TRUE(after.count("tiles/-1_0.jetrees")) << "the stroke made the tile's tree file";
    ASSERT_TRUE(after.count("tiles/0_0.jetrees"));
    EXPECT_FALSE(after.count("tiles/-1_-1.jetrees") || after.count("tiles/0_-1.jetrees")) << "tiles the brush did not reach";
    size_t total = 0;
    expectInsideTiles(after, total);
    EXPECT_EQ(total, static_cast<size_t>(changed));
    // each tree where the brush put it: within 8 m of the stroke's line, on the World's side of its tile
    for (int x = -1; x <= 0; x++) {
        const TreeStore store = treesOfFile(after, TileCoord(x, 0));
        EXPECT_GT(store.size(), 0u) << "tile (" << x << ", 0)";
        for (const TreeRecord& record : store.records()) {
            const float worldX = foliageTreeLocalX(record, TREES_TILE) + (static_cast<float>(x) + 0.5f) * TREES_TILE;
            const float worldZ = foliageTreeLocalZ(record, TREES_TILE) + 32.0f;
            EXPECT_GE(worldX, -20.0f);
            EXPECT_LE(worldX, 20.0f);
            EXPECT_NEAR(worldZ, 16.0f, 8.0f);
            EXPECT_EQ(worldX < 0.0f, x == -1);
        }
    }

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(treesFolderFiles(folder), before) << "one undo puts both tiles back, byte for byte";
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(treesFolderFiles(folder), after) << "and redo the stroke's bytes";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(treesFolderFiles(folder), before);
}

TEST_F(TerrainWorldTreesTest, StrokeOnResidentTileMakesItsFile) {
    // tile (0, -1) in memory with its foliage region and no tree file: the stroke's trees are drawn at
    // once and saved in a file the stroke makes; an erase over the border takes them back out
    ASSERT_TRUE(makeResident());
    const std::map<std::string, std::string> before = treesFolderFiles(folder);
    ASSERT_FALSE(before.count("tiles/0_-1.jetrees"));
    int changed = 0;
    std::string error;
    ASSERT_TRUE(panel->scriptedStroke(scene, world, points({{30, -30}, {34, -30}, {38, -30}}), 6.0f, false, 0, 0.5f, changed, error)) << error;
    ASSERT_GT(changed, 0);
    const std::map<std::string, std::string> painted = treesFolderFiles(folder);
    ASSERT_TRUE(painted.count("tiles/0_-1.jetrees"));
    const std::shared_ptr<TreeStore> drawn = foliage()->getTreeStore(world, Vector3(34.0f, 0.0f, -30.0f));
    ASSERT_NE(drawn, nullptr) << "the tile's region has the stroke's store";
    EXPECT_EQ(drawn->size(), treesOfFile(painted, TileCoord(0, -1)).size());
    EXPECT_EQ(drawn->size(), static_cast<size_t>(changed));

    // a stroke over no tile at all says so and changes nothing
    int none = 0;
    EXPECT_FALSE(panel->scriptedStroke(scene, world, points({{300, 300}}), 6.0f, false, 0, 0.5f, none, error));
    EXPECT_NE(error.find("Trees need ground"), std::string::npos) << error;
    EXPECT_EQ(treesFolderFiles(folder), painted);

    int erased = 0;
    ASSERT_TRUE(panel->scriptedStroke(scene, world, points({{34, -30}}), 20.0f, true, 0, 0.0f, erased, error)) << error;
    EXPECT_EQ(erased, changed);
    EXPECT_EQ(treesOfFile(treesFolderFiles(folder), TileCoord(0, -1)).size(), 0u);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(treesFolderFiles(folder), painted) << "the erase undone";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(treesFolderFiles(folder), before) << "the stroke undone: the file it made is gone";
    const std::shared_ptr<TreeStore> afterUndo = foliage()->getTreeStore(world, Vector3(34.0f, 0.0f, -30.0f));
    EXPECT_TRUE(!afterUndo || afterUndo->empty()) << "and the region draws none";
}

TEST_F(TerrainWorldTreesTest, MassPlaceDistributesPerTile) {
    const std::map<std::string, std::string> before = treesFolderFiles(folder);
    FoliageOpRunner::Request request;
    request.sceneId = scene->id;
    request.terrain = world;
    request.undoName = "Mass Place Trees (Grove)";
    Op op;
    op.type = OpType::Registered;
    op.registeredName = "scatter_by_rules";
    op.seed = 11;
    const auto number = [](double value) { OpParam param; param.kind = OpParam::Kind::Number; param.number = value; return param; };
    OpParam entry;
    entry.kind = OpParam::Kind::Object;
    entry.fields.emplace_back("prototype", number(0));
    OpParam targets;
    targets.kind = OpParam::Kind::List;
    targets.items.push_back(entry);
    OpParam target;
    target.kind = OpParam::Kind::Object;
    target.fields.emplace_back("trees", targets);
    OpParam mode;
    mode.kind = OpParam::Kind::String;
    mode.text = "add";
    op.params.kind = OpParam::Kind::Object;
    op.params.fields.emplace_back("target", target);
    op.params.fields.emplace_back("count", number(400));
    op.params.fields.emplace_back("mode", mode);
    request.ops.push_back(op);

    bool finished = false;
    FoliageOpRunner::Outcome outcome;
    FoliageOpRunner::start(project.get(), request, [&](const FoliageOpRunner::Outcome& done) {
        outcome = done;
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        host.processMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(finished);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;

    const std::map<std::string, std::string> after = treesFolderFiles(folder);
    std::map<uint32_t, size_t> perTile;
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) {
            perTile[TileCoord(x, z).key()] = treesOfFile(after, TileCoord(x, z)).size();
            EXPECT_GT(perTile[TileCoord(x, z).key()], 20u) << "tile (" << x << ", " << z << ") got its share";
            EXPECT_LT(perTile[TileCoord(x, z).key()], 200u) << "tile (" << x << ", " << z << ") got no more than about its share";
        }
    }
    size_t total = 0;
    expectInsideTiles(after, total);
    EXPECT_EQ(static_cast<int64_t>(total), outcome.treesAfter);
    EXPECT_NEAR(static_cast<double>(total), 400.0, 40.0) << "about the count asked for, over the whole World";
    // every tile its own pattern: no two tiles hold the same positions
    EXPECT_NE(treesOfFile(after, TileCoord(-1, -1)).records()[0].u, treesOfFile(after, TileCoord(0, 0)).records()[0].u);

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(treesFolderFiles(folder), before) << "one undo takes the four files back out";
}
