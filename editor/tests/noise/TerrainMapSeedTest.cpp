// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The window T2's generation failed in, held open: a map TerrainMaps creates is asked about by the store before
// any file load could finish. With async loading on and no job worker, a load from the file stays pending
// for as long as the test likes, so only the pixels seeded in the texture pool can answer.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "mcp/TerrainMaps.h"
#include "object/TerrainEdit.h"

#include "support/PosixCompat.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct SeedProject {
        std::unique_ptr<Project> project;
        bool ready = false;
    };

    SeedProject& seedProject() {
        static SeedProject h = []() {
            SeedProject out;
            const fs::path root = fs::temp_directory_path() / ("jasper-terrain-seed-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            std::ofstream(dir / "project.yaml") << "name: terrainseedproject\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            out.ready = out.project->loadProject(dir, false);
            editor::test::releaseAtExit(out.project.get());
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    // Async loading with nothing to run its jobs, for one test. The asset readers take the job system at
    // start(), so they stop and start with it: left on the old pool, a later test's reads end cancelled
    class PendingLoads {
    public:
        PendingLoads(): wasAsync(Engine::isAsyncLoading()) {
            AssetIOService::instance().stop();
            jobs::shutdown();
            jobs::Config config;
            config.workers = 0;
            jobs::initialize(config);
            AssetIOService::instance().start();
            Engine::setAsyncLoading(true);
        }
        ~PendingLoads() {
            Engine::setAsyncLoading(wasAsync);
            AssetIOService::instance().stop();
            jobs::shutdown();
            jobs::initialize(jobs::Config{});
            AssetIOService::instance().start();
        }
    private:
        bool wasAsync;
    };

}

TEST(TerrainMapSeed, ANewBlendMapExistsAtOnce) {
    SeedProject& h = seedProject();
    ASSERT_TRUE(h.ready);
    PendingLoads pending;
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<TerrainComponent>(entity, TerrainComponent());
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(entity);

    mcp::TerrainBlendmapRequest request;
    request.resolution = 33;
    const mcp::TerrainBlendmapResult made = mcp::TerrainMaps::createBlendmap(h.project.get(), 1, entity, request);
    ASSERT_TRUE(made.success) << made.error;
    terrain.blendMaps = {Texture(made.relativePath)};

    TerrainComponentStore store(&scene, entity, terrain);
    const terraincore::MapFormat format = store.format(terraincore::MapId{terraincore::MapKind::Blend, 0});
    EXPECT_TRUE(format.exists) << "the store asked before the file's load could run";
    EXPECT_EQ(format.bounds.w, 33);
}
