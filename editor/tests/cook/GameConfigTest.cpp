// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// game.json carries every Project Setting a game starts with (S98 D1): a Compile build reads it
// as the prebuilt player does (player::runGame), so what the generated main.cpp of a Compile build
// once set is here, and survives the JSON both ways; a game.json from before the new keys keeps
// the engine's defaults.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Project.h"
#include "System.h"
#include "cook/GameConfig.h"
#include "debug/ConsoleCommands.h"
#include "net/Http.h"
#include "player/GameRunner.h"
#include "player/PlayerConfig.h"
#include "registry/PhysicsLayers.h"
#include "render/LightingTiers.h"
#include "registry/SortingLayers.h"
#include "save/SaveGameStore.h"
#include "subsystem/GamepadCursor.h"

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

    // what the craftsman settings change in the engine, at its defaults while a test runs and put
    // back after it
    struct CraftsmanSettingsRestore {
        GamepadCursorSettings cursor = GamepadCursor::getDefaults();
        bool pauseOnFocusLoss = Engine::isPauseOnFocusLoss();
        bool hitTriggers3D = Physics::getQueriesHitTriggers();
        bool hitTriggers2D = Physics2D::getQueriesHitTriggers();
        savegame::SaveSettings saveGame = savegame::settings();
        bool commands = console::commandsEnabled();
        std::string buildProfile = System::getBuildProfile();
        std::vector<std::string> defines = System::getDefines();
        // what applySettings also sets from the rest of game.json
        int canvasWidth = Engine::getCanvasWidth(), canvasHeight = Engine::getCanvasHeight();
        Scaling scaling = Engine::getScalingMode();
        int pixelsPerUnit = Engine::getPixelsPerUnit();
        TextureStrategy textureStrategy = Engine::getTextureStrategy();
        int vsyncCount = Engine::getVSyncCount();
        bool developmentBuild = Engine::isDevelopmentBuild();
        CraftsmanSettingsRestore() { reset(); }
        ~CraftsmanSettingsRestore() {
            Engine::setCanvasSize(canvasWidth, canvasHeight);
            Engine::setScalingMode(scaling);
            Engine::setPixelsPerUnit(pixelsPerUnit);
            Engine::setTextureStrategy(textureStrategy);
            Engine::setVSyncCount(vsyncCount);
            Engine::setDevelopmentBuild(developmentBuild);
            GamepadCursor::setDefaults(cursor);
            Engine::setPauseOnFocusLoss(pauseOnFocusLoss);
            Physics::setQueriesHitTriggers(hitTriggers3D);
            Physics2D::setQueriesHitTriggers(hitTriggers2D);
            savegame::configure(saveGame);
            console::setCommandsEnabled(commands);
            System::setBuildInfo(buildProfile, defines);
        }
        static void reset() {
            GamepadCursor::setDefaults(GamepadCursorSettings());
            Engine::setPauseOnFocusLoss(false);
            Physics::setQueriesHitTriggers(true);
            Physics2D::setQueriesHitTriggers(true);
            savegame::configure(savegame::SaveSettings());
            console::setCommandsEnabled(true);
            System::setBuildInfo("", {});
        }
    };

    // the craftsman features' settings away from their defaults
    player::PlayerConfig craftsmanGame() {
        player::PlayerConfig game;
        game.gamepadCursor.speed = 1500.0f;
        game.gamepadCursor.graphic = "ui/cursor.png";
        game.pauseOnFocusLoss = true;
        game.queriesHitTriggers3D = false;
        game.queriesHitTriggers2D = false;
        game.saveGame.backups = 5;
        game.consoleCommands = "never";
        game.buildProfile = "Demo";
        game.defines = {"DEMO"};
        return game;
    }

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
    htmlUi.imageCacheMiB = 16;   // S93 R56
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
    EXPECT_EQ(game.imageCacheMiB, 16);
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

// S94 F5: Transparency Sort Mode reaches a game as data, every mode (Perspective, the mode older
// projects migrate to, and Orthographic too), where the generated main.cpp once set it
TEST(GameConfig, CarriesTheTransparencySortMode) {
    const TransparencySortMode before = SortingLayers::getTransparencySortMode();
    const Vector3 axis = SortingLayers::getTransparencySortAxis();
    Project project;
    editor::cook::GameConfigTarget target;
    target.fallbackName = "game";
    for (TransparencySortMode mode : {TransparencySortMode::PERSPECTIVE, TransparencySortMode::ORTHOGRAPHIC,
                                      TransparencySortMode::CUSTOM_AXIS, TransparencySortMode::DEFAULT}) {
        project.setTransparencySort(mode, axis);
        EXPECT_EQ(throughJson(editor::cook::makeGameConfig(project, target)).transparencySortMode, mode)
            << SortingLayers::transparencySortModeToken(mode);
    }
    SortingLayers::setTransparencySortMode(before);
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
    // S93's image cache budget: written only away from the platform's default (-1), and read back as it
    EXPECT_FALSE(plain["htmlUi"].contains("imageCacheMiB"));
    EXPECT_EQ(read.imageCacheMiB, -1);
    player::PlayerConfig budget;
    budget.imageCacheMiB = 0;
    EXPECT_EQ(budget.toJson()["htmlUi"]["imageCacheMiB"], 0);
}

TEST(GameConfig, ASourceExportNamesNativePluginsPerTarget) {
    player::PlayerConfig game;
    game.nativePlatformOnlyByTarget["web"] = {{"Pay", "runs only on Android"}};
    game.nativePlatformOnlyByTarget["android"] = {};
    const player::PlayerConfig read = throughJson(game);
    ASSERT_EQ(read.nativePlatformOnlyByTarget.count("web"), 1u);
    EXPECT_EQ(read.nativePlatformOnlyByTarget.at("web").at("Pay"), "runs only on Android");
}

// The settings the craftsman features added to game.json: the project's, or a Build Profile's
// snapshot's, reach every build's game.json (a Compile build's generated init once set them)
TEST(GameConfig, CarriesTheCraftsmanSettings) {
    GameSettingsRestore restore;
    CraftsmanSettingsRestore craftsman;
    Project project;
    GamepadCursorSettings cursor;
    cursor.speed = 1500.0f;
    project.setGamepadCursorSettings(cursor);
    project.getPresentationSettings().pauseOnFocusLoss = true;
    PhysicsQuerySettings queries;
    queries.hitTriggers3D = false;
    project.setPhysicsQuerySettings(queries);
    SaveGameSettings saves;
    saves.backups = 5;
    project.setSaveGameSettings(saves);
    DebugSettings debug;
    debug.consoleCommands = DebugSettings::AllowLua::Never;
    project.setDebugSettings(debug);
    PlayerCrashSettings crash = project.getPlayerCrash();
    crash.journalKiB = 64;
    crash.journalEngineEvents = false;
    project.setPlayerCrash(crash);

    editor::cook::GameConfigTarget target;
    target.fallbackName = "game";
    const player::PlayerConfig game = throughJson(editor::cook::makeGameConfig(project, target));
    EXPECT_FLOAT_EQ(game.gamepadCursor.speed, 1500.0f);
    EXPECT_TRUE(game.pauseOnFocusLoss);
    EXPECT_FALSE(game.queriesHitTriggers3D);
    EXPECT_TRUE(game.queriesHitTriggers2D);
    EXPECT_EQ(game.saveGame.backups, 5);
    EXPECT_EQ(game.consoleCommands, "never");
    EXPECT_EQ(game.crashJournalBytes, 64 << 10);
    EXPECT_FALSE(game.crashJournalEngineEvents);
    EXPECT_TRUE(game.localization.empty()) << "a project without String Tables loads none";

    // a Build Profile's snapshot: its settings, not the open project's
    const std::shared_ptr<Project> snapshot = Project::makeSettingsSnapshot(project);
    snapshot->getPresentationSettings().pauseOnFocusLoss = false;
    target.settings = snapshot.get();
    EXPECT_FALSE(editor::cook::makeGameConfig(project, target).pauseOnFocusLoss);
    EXPECT_EQ(editor::cook::makeGameConfig(project, target).saveGame.backups, 5);
}

// player::applySettings, which the prebuilt player and a Compile build's main.cpp both run
// (player::runGame), applies them before the first scene
TEST(GameConfig, ApplySettingsAppliesTheCraftsmanSettings) {
    CraftsmanSettingsRestore restore;
    player::applySettings(craftsmanGame());
    EXPECT_FLOAT_EQ(GamepadCursor::getDefaults().speed, 1500.0f);
    EXPECT_EQ(GamepadCursor::getDefaults().graphic, "ui/cursor.png");
    EXPECT_TRUE(Engine::isPauseOnFocusLoss());
    EXPECT_FALSE(Physics::getQueriesHitTriggers());
    EXPECT_FALSE(Physics2D::getQueriesHitTriggers());
    EXPECT_EQ(savegame::settings().backups, 5);
    EXPECT_FALSE(console::commandsEnabled()) << "Console Commands: never";
    EXPECT_EQ(System::getBuildProfile(), "Demo");
    EXPECT_TRUE(System::isDefined("DEMO"));

    // the defaults put them back (a game.json without the keys)
    player::applySettings(player::PlayerConfig());
    EXPECT_TRUE(GamepadCursor::getDefaults() == GamepadCursorSettings());
    EXPECT_FALSE(Engine::isPauseOnFocusLoss());
    EXPECT_TRUE(Physics::getQueriesHitTriggers());
    EXPECT_EQ(savegame::settings().backups, savegame::SaveSettings().backups);
}
