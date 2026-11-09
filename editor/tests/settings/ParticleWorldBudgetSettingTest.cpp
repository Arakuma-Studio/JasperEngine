// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// BF23 F3: Project Settings > Graphics > World Particle Budget (particles.world_budget) reaches
// ParticleWorld (S20 R15); left at 0 the platform's default stays, so old projects behave the same.

#include "Generator.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "Workspace.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleWorld.h"
#include "player/PlayerConfig.h"
#include "settings/SettingsRegistry.h"
#include "subsystem/ParticleSubsystem.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the budget a frame of a scene's particle world ran with
    uint32_t budgetOfAFrame(){
        Scene scene;
        ParticleSystem system(&scene);
        EXPECT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "seed": 4, "maxParticles": 10, "modules": [
            {"type": "main", "startLifetime": 1}, {"type": "emission", "rateOverTime": 10}]})"));
        ParticleSubsystem& particles = *scene.getSystem<ParticleSubsystem>();
        particles.update(1.0 / 60.0);
        return particles.getWorldStats().budget;
    }

}

TEST(ParticleWorldBudgetSetting, AProjectSettingReachesTheWorld){
    EXPECT_EQ(budgetOfAFrame(), particles::defaultWorldBudget());

    const SettingRow* row = SettingsRegistry::get().find("particles.world_budget");
    ASSERT_NE(row, nullptr);
    Project project;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    std::string error;
    ASSERT_TRUE(row->set(ctx, mcp::Json(1000), true, error)) << error;
    EXPECT_EQ(budgetOfAFrame(), 1000u);
    EXPECT_EQ(row->get(ctx), mcp::Json(1000));

    ASSERT_TRUE(row->set(ctx, mcp::Json(0), true, error)) << error;
    EXPECT_EQ(budgetOfAFrame(), particles::defaultWorldBudget());
    EXPECT_FALSE(row->set(ctx, mcp::Json(-1), true, error));
}

TEST(ParticleWorldBudgetSetting, RoundTripsAndAnOldProjectKeepsItsDefault){
    Project project;
    EXPECT_FALSE(Stream::encodeProject(&project)["particleWorldBudget"]) << "a default writes nothing: old files stay as they were";
    project.setParticleWorldBudget(1234);
    const std::string yaml = YAML::Dump(Stream::encodeProject(&project));
    project.setParticleWorldBudget(0);

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.getParticleWorldBudget(), 1234u) << yaml;
    EXPECT_EQ(particles::ParticleWorld::projectBudget(), 1234u) << "read at load, by the editor's scenes";

    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\n"), Workspace());
    EXPECT_EQ(old.getParticleWorldBudget(), 0u);
    EXPECT_EQ(particles::ParticleWorld::projectBudget(), 0u);
}

TEST(ParticleWorldBudgetSetting, ReachesTheGames){
    namespace fs = std::filesystem;
    const fs::path project = fs::temp_directory_path() / ("jasper-generator-particles-" + std::to_string(::getpid()));
    fs::remove_all(project);
    fs::create_directories(project / "assets");
    const fs::path internal = project / ".jasperengine";
    auto generatedMain = [&](uint32_t budget){
        Generator generator;
        const std::vector<SceneBuildInfo> scenes = {SceneBuildInfo{1, "Level", {1}, {1}, true}};
        generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17,
                            Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(),
                            TagsAndLayers(), 4, ThreadingProjectSettings(), HtmlUiRenderSettings(), PbrTier::REDUCED,
                            ShaderCreationProjectSettings(), false, {}, PlayerSettings(), AudioSettings(), DebugSettings(), budget);
        std::ifstream file(internal / "generated" / "main.cpp");
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    };
    const std::string set = generatedMain(1000);
    const size_t call = set.find("ParticleWorld::setProjectBudget(1000);");
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(call, set.find("SceneManager::loadScene")) << "before the first scene";
    // an exported project's CMake does not define JASPERENGINE_PROJECT_PATH: the header has to be
    // outside that block, or a compiled export does not build (the smoke test compiles one)
    const size_t include = set.find("#include \"particles/ParticleWorld.h\"");
    ASSERT_NE(include, std::string::npos);
    EXPECT_LT(include, set.find("#ifdef JASPERENGINE_PROJECT_PATH"));
    EXPECT_EQ(generatedMain(0).find("setProjectBudget"), std::string::npos) << "the default costs a game nothing";
    fs::remove_all(project);

    // a prebuilt player reads it from its configuration
    player::PlayerConfig game;
    game.particleWorldBudget = 1000;
    player::PlayerConfig read;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(game.toJson(), read, error)) << error;
    EXPECT_EQ(read.particleWorldBudget, 1000u);
    ASSERT_TRUE(player::PlayerConfig::fromJson(player::PlayerConfig().toJson(), read, error)) << error;
    EXPECT_EQ(read.particleWorldBudget, 0u);
}
