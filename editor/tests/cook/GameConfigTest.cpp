// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// game.json carries every Project Setting a game starts with (S98 D1): a Compile build reads it
// as the prebuilt player does (player::runGame), so what the generated main.cpp of a Compile build
// once set is here, and survives the JSON both ways; a game.json from before the new keys keeps
// the engine's defaults.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Project.h"
#include "cook/GameConfig.h"
#include "net/Http.h"
#include "player/PlayerConfig.h"
#include "render/LightingTiers.h"

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    player::PlayerConfig throughJson(const player::PlayerConfig& game) {
        player::PlayerConfig read;
        std::string error;
        EXPECT_TRUE(player::PlayerConfig::fromJson(nlohmann::json::parse(game.toJson().dump()), read, error)) << error;
        return read;
    }

    // the engine state the Project setters change, put back after a test
    struct GameSettingsRestore {
        LightMode lightMode = LightingTiers::getLightMode();
        bool gpuInstancing = Engine::isGpuInstancingEnabled();
        bool dynamicBatching = Engine::isDynamicBatching();
        bool insecureHttp = Http::isInsecureHttpAllowed();
        int targetFrameRate = Engine::getTargetFrameRate();
        ~GameSettingsRestore() {
            LightingTiers::setLightMode(lightMode);
            Engine::setGpuInstancingEnabled(gpuInstancing);
            Engine::setDynamicBatchingDefault(dynamicBatching);
            Http::setInsecureHttpAllowed(insecureHttp);
            Engine::setTargetFrameRate(targetFrameRate);
        }
    };

}

TEST(GameConfig, CarriesWhatTheGeneratedInitSet) {
    GameSettingsRestore restore;
    Project project;
    DrawSubmissionSettings draw;
    draw.gpuInstancing = false;
    project.setDrawSubmissionSettings(draw);
    project.setLightMode(LightMode::TILED);
    project.setDynamicBatching(false);
    HtmlUiRenderSettings htmlUi;
    htmlUi.glyphAtlasMiB = 24;
    htmlUi.sdfPrimitives = false;
    project.setHtmlUiRenderSettings(htmlUi);
    project.setParticleWorldBudget(1000);
    CanvasScalerSettings scaler;
    scaler.mode = UIScaleMode::ScaleWithScreenSize;
    scaler.referenceResolution = Vector2(1280.0f, 720.0f);
    project.setUIScale(scaler);
    Physics3DWorldSettings world;
    world.maxBodies = 4096;
    project.setPhysics3DWorldSettings(world);
    ShaderCreationProjectSettings creation;
    creation.asyncCreation = false;
    creation.mainThreadBudgetMs = 4.0f;
    project.setShaderCreationSettings(creation);
    ShaderGlobalDecl fog;
    fog.name = "FogTint";
    fog.type = ShaderGlobalType::Color;
    fog.defaultValue = {Vector4(0.5f, 0.25f, 1.0f, 1.0f)};
    project.setShaderGlobals({fog});
    DebugSettings debug;
    debug.overlay = DebugSettings::Mode::AllBuilds;
    debug.statsKey = "f4";
    debug.logBufferSize = 900;
    project.setDebugSettings(debug);
    project.getPresentationSettings().targetFrameRate = 30;
    project.getPresentationSettings().keepScreenOn = false;
    project.getApplicationSettings().allowInsecureHttp = true;

    editor::cook::GameConfigTarget target;
    target.fallbackName = "game";
    const player::PlayerConfig game = throughJson(editor::cook::makeGameConfig(project, target));

    EXPECT_FALSE(game.gpuInstancing);
    EXPECT_EQ(game.lightMode, LightMode::TILED);
    EXPECT_FALSE(game.dynamicBatching);
    EXPECT_EQ(game.glyphAtlasMiB, 24);
    EXPECT_FALSE(game.sdfPrimitives);
    EXPECT_EQ(game.particleWorldBudget, 1000u);
    EXPECT_TRUE(game.canvasScaler == scaler);
    ASSERT_TRUE(game.hasPhysics3DCapacity);
    EXPECT_EQ(game.maxBodies3D, 4096u);
    EXPECT_FALSE(game.shaderCreation.asyncCreation);
    EXPECT_FLOAT_EQ(game.shaderCreation.mainThreadBudgetMs, 4.0f);
    ASSERT_EQ(game.shaderGlobals.size(), 1u);
    EXPECT_TRUE(game.shaderGlobals[0] == fog);
    ASSERT_TRUE(game.hasDebugSettings);
    EXPECT_EQ(game.debugOverlayMode, player::PlayerConfig::BuildMode::Always);
    EXPECT_EQ(game.debugOverlay.statsKey, "f4");
    EXPECT_EQ(game.logBufferSize, 900u);
    ASSERT_TRUE(game.hasPlayerSettings);
    EXPECT_EQ(game.targetFrameRate, 30);
    EXPECT_FALSE(game.keepScreenOn);
    EXPECT_TRUE(game.allowInsecureHttp);
    // a source export's native plugins go per target: none here
    EXPECT_TRUE(game.nativePlatformOnly.empty());
}

TEST(GameConfig, AnOlderGameJsonKeepsTheEngineDefaults) {
    player::PlayerConfig game;
    game.hasPlayerSettings = true;
    game.hasDebugSettings = true;
    game.hasPhysics3DCapacity = true;
    game.gpuInstancing = false;
    game.nativePlatformOnlyByTarget["web"] = {{"Pay", "runs only on Android"}};
    nlohmann::json json = game.toJson();
    for (const char* key : {"player", "debug", "physics3D", "gpuInstancing", "shaderCreation", "shaderGlobals", "nativePlatformOnlyByTarget"})
        json.erase(key);
    player::PlayerConfig read;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(json, read, error)) << error;
    EXPECT_FALSE(read.hasPlayerSettings);
    EXPECT_FALSE(read.hasDebugSettings);
    EXPECT_FALSE(read.hasPhysics3DCapacity);
    EXPECT_TRUE(read.gpuInstancing);
    EXPECT_TRUE(read.shaderGlobals.empty());
    EXPECT_TRUE(read.nativePlatformOnlyByTarget.empty());
    EXPECT_TRUE(read.shaderCreation == ShaderCreationSettings());
    // the defaults are not written: a game without them keeps its file
    const nlohmann::json plain = player::PlayerConfig().toJson();
    for (const char* key : {"player", "debug", "physics3D", "gpuInstancing", "shaderCreation", "shaderGlobals", "nativePlatformOnlyByTarget"})
        EXPECT_FALSE(plain.contains(key)) << key;
    EXPECT_FALSE(plain["canvas"].contains("scaler"));
}

TEST(GameConfig, ASourceExportNamesNativePluginsPerTarget) {
    player::PlayerConfig game;
    game.nativePlatformOnlyByTarget["web"] = {{"Pay", "runs only on Android"}};
    game.nativePlatformOnlyByTarget["android"] = {};
    const player::PlayerConfig read = throughJson(game);
    ASSERT_EQ(read.nativePlatformOnlyByTarget.count("web"), 1u);
    EXPECT_EQ(read.nativePlatformOnlyByTarget.at("web").at("Pay"), "runs only on Android");
}
