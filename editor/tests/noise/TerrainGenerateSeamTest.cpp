// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 R61, AC44: what T1's Create Terrain dialog does with this spec, through a fake dialog: it lists
// TerrainPresets::list() with each preset's recommended size and height, and once it has made the terrain,
// calls TerrainGeneration::start. T1 owns the dialog; these two calls are T2's seam.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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

    // The shape of T1's dialog: what it shows, and what it does on Create
    struct FakeCreateDialog {
        std::vector<TerrainPreset> offered = TerrainPresets::list();
        TerrainPreset chosen;
        float terrainSize = 0.0f;
        float maxHeight = 0.0f;

        void choose(const TerrainPreset& preset) {
            chosen = preset;
            terrainSize = preset.terrainSize;
            maxHeight = preset.maxHeight;
        }

        std::shared_ptr<TerrainGeneration::Run> create(Project* project, uint32_t sceneId, Entity entity, TerrainGeneration::Done done) const {
            GenerationRequest request;
            graph::LoadReport report;
            request.graph = graph::load(TerrainPresets::json(chosen.id), report);
            EXPECT_TRUE(report.ok) << report.error;
            request.displayName = chosen.name;
            request.seed = 7;
            request.resolution = 33;
            return TerrainGeneration::start(project, sceneId, entity, std::move(request), std::move(done));
        }
    };

}

TEST(TerrainGeneration, CreateDialogSeam) {
    noise::registerNoiseKind();
    Project* project = seamProject();
    project->createNewScene("Create Terrain", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    FakeCreateDialog dialog;
    ASSERT_FALSE(dialog.offered.empty());
    for (const TerrainPreset& preset : dialog.offered) {
        EXPECT_FALSE(preset.hidden) << preset.id << ": the dialog offers no utility graph";
        EXPECT_GT(preset.terrainSize, 0.0f) << preset.id;
        EXPECT_GT(preset.maxHeight, 0.0f) << preset.id;
    }
    // the graph with the fewest words: a seam test, not a performance one
    const TerrainPreset* simple = &dialog.offered.front();
    for (const TerrainPreset& preset : dialog.offered) {
        if (TerrainPresets::json(preset.id).size() < TerrainPresets::json(simple->id).size()) simple = &preset;
    }
    dialog.choose(*simple);

    Entity entity = scene->createUserEntity();
    scene->setEntityName(entity, "Terrain");
    scene->addComponent<Transform>(entity, {});
    TerrainComponent terrain;
    terrain.terrainSize = dialog.terrainSize;
    terrain.maxHeight = dialog.maxHeight;
    scene->addComponent<TerrainComponent>(entity, terrain);
    sceneProject->entities.push_back(entity);

    bool called = false;
    GenerationResult got;
    std::shared_ptr<TerrainGeneration::Run> run = dialog.create(project, sceneId, entity, [&](const GenerationResult& result) {
        called = true;
        got = result;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!called && std::chrono::steady_clock::now() < deadline) {
        jobs::drainMainQueue();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(called);
    EXPECT_TRUE(got.ok) << got.error;
    EXPECT_NE(got.undoName.find(dialog.chosen.name), std::string::npos) << got.undoName;
    EXPECT_NE(got.undoName.find("seed 7"), std::string::npos) << got.undoName;
    EXPECT_FALSE(scene->getComponent<TerrainComponent>(entity).heightMap.empty()) << "the map exists and was written";
}
