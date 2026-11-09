// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Import Heightmap on a Terrain World as a job (T4 R32, R41; AC29's undo and cancel): the swap is one
// undo step that puts the files back byte for byte, both ways, and a cancel leaves the world's folder
// and the project's trash as they were.

#include "../terrain/TerrainProjectFixture.h"

#include "EditorHost.h"
#include "component/TerrainWorldComponent.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrainworld/TerrainWorldEditor.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "terrainworld/TerrainWorldImport.h"
#include "util/FileUtils.h"

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace terrainprojecttest;
using namespace jasperengine::terraincore;

namespace {

    // The App's side of enqueueMainThreadTask: tasks wait for the next editor frame
    struct ImportLoopHost : EditorHost {
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

    std::map<std::string, std::string> importFolderFiles(const fs::path& folder) {
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

    class TerrainWorldImportJobTest : public TerrainProjectFixture {
    protected:
        ImportLoopHost host;
        Entity world = NULL_ENTITY;
        fs::path folder;

        void SetUp() override {
            TerrainProjectFixture::SetUp();
            setEditorHost(&host);
            installTerrainWorldHost(project.get());
            TerrainWorldSettings settings;
            settings.name = "Imported";
            settings.tileQuads = 64;
            settings.maxHeight = 32.0f;
            std::string error;
            world = createTerrainWorld(project.get(), scene->id, NULL_ENTITY, settings, error);
            ASSERT_NE(world, NULL_ENTITY) << error;
            folder = project->resolveAssetPath(FileUtils::pathFromUtf8(scene->scene->getComponent<TerrainWorldComponent>(world).worldData.path));
            // 129 x 129 16-bit samples, a ramp along x: tiles (0, 0) to (1, 1), three of them new
            std::ofstream raw(project->getAssetsPath() / "ramp.r16", std::ios::binary);
            for (int z = 0; z < 129; z++) {
                for (int x = 0; x < 129; x++) {
                    const uint16_t value = static_cast<uint16_t>(x * 300 + z * 7);
                    raw.put(static_cast<char>(value & 0xFF));
                    raw.put(static_cast<char>(value >> 8));
                }
            }
        }

        void TearDown() override {
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

        TerrainWorldImportPlan plan() {
            TerrainWorldImportRequest request;
            request.path = "ramp.r16";
            TerrainWorldImportPlan out;
            std::string error;
            EXPECT_TRUE(planTerrainWorldImport(project.get(), *runtime(), request, out, error)) << error;
            return out;
        }

        // editor frames until the import says it is done
        bool runUntil(const bool& called) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!called && std::chrono::steady_clock::now() < deadline) {
                host.processMainThreadTasks();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return called;
        }

        fs::path stashRoot() const { return project->getProjectInternalPath() / "trash" / "terrain-world-import"; }
    };

}

TEST_F(TerrainWorldImportJobTest, UndoRedoByteIdentical) {
    const TerrainWorldImportPlan made = plan();
    EXPECT_EQ(made.width, 129);
    EXPECT_TRUE(made.banded) << "a 16-bit raw is read in bands";
    EXPECT_EQ(made.covered.size(), 4u);
    EXPECT_EQ(made.made, 3);
    EXPECT_EQ(made.changed.size(), 7u) << "the covered tiles and the three that were there beside them";
    const auto before = importFolderFiles(folder);
    const size_t steps = history()->position();

    bool called = false;
    TerrainWorldImportResult result;
    std::shared_ptr<TerrainWorldImportTask> task = startTerrainWorldImport(project.get(), scene->id, world, made, [&](const TerrainWorldImportResult& r) {
        result = r;
        called = true;
    });
    ASSERT_TRUE(runUntil(called));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(task->finished());
    EXPECT_EQ(history()->position(), steps + 1) << "one undo step";
    const auto after = importFolderFiles(folder);
    EXPECT_NE(after, before);
    EXPECT_TRUE(after.count("tiles/1_1.jtile"));
    // the file's samples, normalized over its range: x = 64 is at 64 * 300 + 7 * z of 128 * 300 + 7 * 128
    TerrainWorldRuntime* live = runtime();
    ASSERT_TRUE(live);
    const float expected = 32.0f * (64.0f * 300.0f + 7.0f * 10.0f) / (128.0f * 300.0f + 7.0f * 128.0f);
    EXPECT_NEAR(live->query(64.0f, 10.0f, true).height, expected, 0.01f);

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(importFolderFiles(folder), before) << "undo puts every file back, byte for byte";
    EXPECT_EQ(live->tileState(TileCoord(1, 1)), TileState::Absent);
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(importFolderFiles(folder), after) << "redo writes the import's files again";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(importFolderFiles(folder), before);
}

TEST_F(TerrainWorldImportJobTest, CancelLeavesFolderUntouched) {
    const auto before = importFolderFiles(folder);
    const size_t steps = history()->position();
    bool called = false;
    TerrainWorldImportResult result;
    std::shared_ptr<TerrainWorldImportTask> task = startTerrainWorldImport(project.get(), scene->id, world, plan(), [&](const TerrainWorldImportResult& r) {
        result = r;
        called = true;
    });
    // before the swap runs on the main thread, wherever the job has got to
    task->cancel();
    ASSERT_TRUE(runUntil(called));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(history()->position(), steps) << "no undo step";
    EXPECT_EQ(importFolderFiles(folder), before) << "the world's folder is untouched";
    std::error_code ec;
    EXPECT_TRUE(!fs::exists(stashRoot(), ec) || fs::is_empty(stashRoot(), ec)) << "the job's own tiles are gone";
}

TEST_F(TerrainWorldImportJobTest, EditDuringTheJobRefusesTheSwap) {
    const TerrainWorldImportPlan made = plan();
    const size_t steps = history()->position();
    bool called = false;
    TerrainWorldImportResult result;
    startTerrainWorldImport(project.get(), scene->id, world, made, [&](const TerrainWorldImportResult& r) {
        result = r;
        called = true;
    });
    // a tile the import changes is edited and saved before the swap: the import would lose it
    TerrainWorldRuntime* live = runtime();
    ASSERT_TRUE(live);
    const int m = live->lattice().heightOrigin();
    std::vector<float> values(4, 0.9f);
    ASSERT_TRUE(live->store().write(MapId{MapKind::Height, 0}, TexelRect{m + 10, m + 10, 2, 2}, values).ok);
    ASSERT_TRUE(TerrainWorldFiles::flushWorld(*live));
    const auto edited = importFolderFiles(folder);
    ASSERT_TRUE(runUntil(called));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("changed while"), std::string::npos) << result.error;
    EXPECT_EQ(history()->position(), steps);
    EXPECT_EQ(importFolderFiles(folder), edited);
}
