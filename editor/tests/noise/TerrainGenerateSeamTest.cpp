// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 R61, AC44: T1's Create Terrain dialog offers TerrainPresets::list() with each preset's recommended size and
// height, and once it has made the terrain, runs the preset through TerrainGeneration::start. T1 owns the dialog;
// these two calls are T2's seam.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"
#include "object/TerrainEdit.h"
#include "window/dialog/CreateTerrainDialog.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    Project* seamProject() {
        static std::unique_ptr<Project> project = []() {
            const fs::path root = fs::temp_directory_path() / ("jasper-terrain-seam-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            fs::create_directories(dir / "assets");
            std::ofstream(dir / "project.yaml") << "name: terrainseamproject\nassetsDir: assets\n";
            AppSettings::initialize();
            auto out = std::make_unique<Project>();
            editor::test::useProjectPaths(out.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            EXPECT_TRUE(out->loadProject(dir, false));
            editor::test::releaseAtExit(out.get());
            return out;
        }();
        editor::test::useProjectPaths(project.get());
        return project.get();
    }

    // The App's side of enqueueMainThreadTask: a task waits for the main loop's next frame, and a post from a
    // worker wakes the loop. Its frames never drain jobs::postToMain, as an editor that draws no scene does not.
    struct LoopHost : EditorHost {
        std::mutex mutex;
        std::vector<std::function<void()>> tasks;
        std::atomic<int> wakes{0};

        void enqueueMainThreadTask(std::function<void()> task) override {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
            wakes++;
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

    struct HostScope {
        explicit HostScope(EditorHost& host) { setEditorHost(&host); }
        ~HostScope() { setEditorHost(nullptr); }
    };

    // Idle editor frames until called, at most `seconds`
    void runFramesUntil(LoopHost& host, const bool& called, int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!called && std::chrono::steady_clock::now() < deadline) {
            host.processMainThreadTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    Entity addTerrain(Scene* scene, SceneProject* sceneProject, float terrainSize, float maxHeight) {
        Entity entity = scene->createUserEntity();
        scene->setEntityName(entity, "Terrain");
        scene->addComponent<Transform>(entity, {});
        TerrainComponent terrain;
        terrain.terrainSize = terrainSize;
        terrain.maxHeight = maxHeight;
        scene->addComponent<TerrainComponent>(entity, terrain);
        sceneProject->entities.push_back(entity);
        return entity;
    }

    // The preset with the fewest words: seam tests, not performance ones
    TerrainPreset simplestPreset(const std::vector<TerrainPreset>& presets) {
        const TerrainPreset* simple = &presets.front();
        for (const TerrainPreset& preset : presets) {
            if (TerrainPresets::json(preset.id).size() < TerrainPresets::json(simple->id).size()) simple = &preset;
        }
        return *simple;
    }

}

TEST(TerrainGeneration, CreateDialogSeam) {
    noise::registerNoiseKind();
    Project* project = seamProject();
    project->createNewScene("Create Terrain", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    const std::vector<TerrainPreset> offered = TerrainPresets::list();
    ASSERT_FALSE(offered.empty());
    for (const TerrainPreset& preset : offered) {
        EXPECT_FALSE(preset.hidden) << preset.id << ": the dialog offers no utility graph";
        EXPECT_GT(preset.terrainSize, 0.0f) << preset.id;
        EXPECT_GT(preset.maxHeight, 0.0f) << preset.id;
    }
    const TerrainPreset chosen = simplestPreset(offered);
    CreateTerrainDialog::Settings settings;
    settings.name = "Seam";
    settings.size = chosen.terrainSize;
    settings.height = chosen.maxHeight;
    settings.heightmap = 33;
    settings.noisePreset = chosen.id;
    settings.seed = 7;
    std::string error;
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t before = history->position();

    LoopHost host;
    HostScope scope(host);
    bool called = false;
    Entity entity = NULL_ENTITY;
    GenerationResult got;
    ASSERT_TRUE(CreateTerrainDialog::createGenerated(project, sceneId, NULL_ENTITY, settings, [&](Entity made, const GenerationResult& result) {
        called = true;
        entity = made;
        got = result;
    }, error)) << error;
    runFramesUntil(host, called, 120);
    ASSERT_TRUE(called);
    ASSERT_NE(entity, NULL_ENTITY) << got.error;
    EXPECT_TRUE(got.ok) << got.error;
    EXPECT_NE(got.undoName.find(chosen.name), std::string::npos) << got.undoName;
    EXPECT_NE(got.undoName.find("seed 7"), std::string::npos) << got.undoName;
    EXPECT_LT(got.stats.heightMin, got.stats.heightMax) << "the preset shaped the flat map";
    {
        const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
        EXPECT_EQ(terrain.terrainSize, chosen.terrainSize);
        EXPECT_EQ(terrain.maxHeight, chosen.maxHeight);
    }
    const auto heightBytes = [&]() {
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
        TerrainComponentStore store(scene, entity, terrain);
        const terraincore::MapId height{terraincore::MapKind::Height, 0};
        const terraincore::MapFormat format = store.format(height);
        std::vector<uint8_t> bytes(static_cast<size_t>(format.bounds.area()) * static_cast<size_t>(format.channels) *
                                   static_cast<size_t>(format.bitsPerChannel / 8));
        EXPECT_TRUE(store.readBytes(height, format.bounds, bytes));
        return bytes;
    };
    const std::vector<uint8_t> generated = heightBytes();

    // one click, one step: Create and the preset's generation, under the preset's name
    EXPECT_EQ(history->position(), before + 1) << "Create Terrain with a preset is one undo step";
    ASSERT_NE(history->undoCommand(), nullptr);
    EXPECT_EQ(history->undoCommand()->name(), "Create Terrain (" + chosen.name + ")");
    EXPECT_TRUE(history->undo());
    EXPECT_EQ(history->position(), before);
    EXPECT_FALSE(scene->isEntityCreated(entity) && scene->findComponent<TerrainComponent>(entity)) << "one undo removes the terrain";
    EXPECT_TRUE(history->redo());
    ASSERT_TRUE(scene->isEntityCreated(entity) && scene->findComponent<TerrainComponent>(entity)) << "one redo brings it back";
    EXPECT_EQ(heightBytes(), generated) << "with the generated heights";
}

// The Generate section's Preview in an editor that draws nothing else: its result comes back through the editor's
// own queue, on the main thread, and wakes the loop. Through jobs::postToMain it waited for an unrelated redraw.
TEST(TerrainGeneration, PreviewResultReachesAnIdleEditor) {
    noise::registerNoiseKind();
    Project* project = seamProject();
    project->createNewScene("Idle Preview", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    const TerrainPreset preset = simplestPreset(TerrainPresets::list());
    const Entity entity = addTerrain(sceneProject->scene, sceneProject, preset.terrainSize, preset.maxHeight);

    GenerationRequest request;
    graph::LoadReport report;
    request.graph = graph::load(TerrainPresets::json(preset.id), report);
    ASSERT_TRUE(report.ok) << report.error;
    request.displayName = preset.name;
    request.resolution = 33;
    request.dryRun = true;
    request.interactive = true;

    LoopHost host;
    HostScope scope(host);
    const std::thread::id mainThread = std::this_thread::get_id();
    bool called = false;
    bool onMain = false;
    GenerationResult got;
    std::shared_ptr<TerrainGeneration::Run> run = TerrainGeneration::start(project, sceneId, entity, std::move(request), [&](const GenerationResult& result) {
        called = true;
        onMain = std::this_thread::get_id() == mainThread;
        got = result;
    });
    ASSERT_TRUE(run);
    runFramesUntil(host, called, 10);
    ASSERT_TRUE(called) << "the result never reached the editor's loop";
    EXPECT_TRUE(onMain);
    EXPECT_GE(host.wakes.load(), 1) << "a result posted from the job wakes an idle loop";
    EXPECT_TRUE(got.ok) << got.error;
    EXPECT_TRUE(got.dryRun);
    EXPECT_TRUE(run->finished());
}

// R28's "always, once": a job that throws ends the run with its error, which the Generate section shows as its
// status, instead of a progress bar that never ends
TEST(TerrainGeneration, AJobThatThrowsReportsItsError) {
    noise::registerNoiseKind();
    Project* project = seamProject();
    project->createNewScene("Throwing Job", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    const TerrainPreset preset = simplestPreset(TerrainPresets::list());
    const Entity entity = addTerrain(sceneProject->scene, sceneProject, preset.terrainSize, preset.maxHeight);

    GenerationRequest request;
    graph::LoadReport report;
    request.graph = graph::load(TerrainPresets::json(preset.id), report);
    ASSERT_TRUE(report.ok) << report.error;
    request.displayName = preset.name;
    request.resolution = 33;
    request.dryRun = true;

    LoopHost host;
    HostScope scope(host);
    struct FaultScope {
        FaultScope() { TerrainGeneration::setJobFaultForTests([]() { throw std::runtime_error("out of memory"); }); }
        ~FaultScope() { TerrainGeneration::setJobFaultForTests(nullptr); }
    } fault;
    bool called = false;
    int calls = 0;
    GenerationResult got;
    std::shared_ptr<TerrainGeneration::Run> run = TerrainGeneration::start(project, sceneId, entity, std::move(request), [&](const GenerationResult& result) {
        called = true;
        calls++;
        got = result;
    });
    ASSERT_TRUE(run);
    runFramesUntil(host, called, 10);
    ASSERT_TRUE(called) << "a job that threw never reported";
    host.processMainThreadTasks();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(got.ok);
    EXPECT_FALSE(got.cancelled);
    EXPECT_EQ(got.errorCode, "evaluate-failed");
    EXPECT_NE(got.error.find("out of memory"), std::string::npos) << got.error;
    EXPECT_TRUE(run->finished());
}
