// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/CppIdentifier.h"
#include "GameConfig.h"

#include "DebugSettings.h"
#include "Project.h"
#include "render/shadow/ShadowTiers.h"
#include "terrain/TerrainQuality.h"
#include "util/NativeNeeds.h"
#include "util/NativePlugins.h"

using namespace jasperengine;
using namespace jasperengine::editor;

player::PlayerConfig editor::cook::makeGameConfig(Project& project, const GameConfigTarget& target) {
    // the settings of a Build Profile's snapshot, else the project's; the scenes are the project's
    Project& settings = target.settings ? *target.settings : project;
    player::PlayerConfig game;
    const WindowSettings window = settings.getWindowSettings();
    game.name = settings.getName().empty() ? target.fallbackName : settings.getName();
    if (target.platform) {
        const BuildPlatform platform = *target.platform;
        game.appId = platform == BuildPlatform::Android ? settings.getAndroidProjectSettings().packageName
                   : platform == BuildPlatform::MacOS ? settings.getMacOSProjectSettings().bundleIdentifier
                   : platform == BuildPlatform::IOS ? settings.getIOSProjectSettings().bundleIdentifier
                   : target.fallbackName;
    }
    if (game.appId.empty()) game.appId = target.fallbackName;
    game.version = settings.getAndroidProjectSettings().versionName;
    // the native plugins the build's platform has no code of
    const std::vector<NativePlugins::PluginInfo> plugins = NativePlugins::listPlugins(NativePlugins::scan(project.getProjectPath()));
    if (target.platform) {
        game.nativePlatformOnly = NativePlugins::platformOnlyPlugins(plugins, nativeRunningName(*target.platform));
    } else {
        for (const char* running : {"android", "ios", "macos", "macos-cmake", "web", "desktop"}) {
            std::map<std::string, std::string> only = NativePlugins::platformOnlyPlugins(plugins, running);
            if (!only.empty()) game.nativePlatformOnlyByTarget[running] = std::move(only);
        }
    }
    game.startScene = target.startScene != 0 ? target.startScene : project.getStartSceneId();
    for (const SceneProject& scene : project.getScenes()) {
        if (!scene.inBuild) continue;   // opened without being listed (S98 REQ-28)
        if (target.scenes && !target.scenes->count(scene.id)) continue;
        player::GameScene item;
        item.id = scene.id;
        item.name = scene.name;
        item.file = "scenes/" + std::to_string(scene.id) + ".jscn";
        project.collectInvolvedScenesForExport(scene.id, item.involved);
        project.collectStartActiveScenesForExport(scene.id, item.active);
        game.scenes.push_back(std::move(item));
    }
    if (!game.findScene(game.startScene) && !game.scenes.empty()) game.startScene = game.scenes.front().id;
    for (const CookedPrefabInfo& prefab : target.prefabs) game.prefabs.push_back({prefab.id, prefab.name, prefab.guid, prefab.file});
    game.windowMode = static_cast<int>(window.mode);
    game.windowWidth = static_cast<int>(window.width);
    game.windowHeight = static_cast<int>(window.height);
    game.resizable = window.resizable;
    game.title = window.title.empty() ? game.name : window.title;
    game.vsync = settings.isVSyncEnabled();
    game.storageId = toIdentifier(game.name);
    game.canvasWidth = settings.getCanvasWidth();
    game.canvasHeight = settings.getCanvasHeight();
    game.scaling = settings.getScalingMode();
    game.pixelsPerUnit = settings.getPixelsPerUnit();
    game.defaultFilterPoint = settings.getDefaultFilterMode() == TextureFilterMode::Point;
    game.bitmapTextCacheMiB = settings.getBitmapTextCacheMiB();
    game.textureStrategy = settings.getTextureStrategy();
    game.canvasScaler = settings.getUIScale();
    const LoadingSettings& loading = settings.getLoadingSettings();
    game.loadingScene = loading.sceneId;
    game.loadingDelay = loading.delay;
    game.loadingTimeout = loading.timeout;
    game.asyncLoading = loading.asyncLoading;
    game.particleWorldBudget = settings.getParticleWorldBudget();
    const ThreadingProjectSettings& threading = settings.getThreadingSettings();
    game.threading.workers = threading.workers;
    game.threading.reservedFrameWorkers = threading.reservedFrameWorkers;
    game.threading.ioThreads = threading.ioThreads;
    game.threading.webPumpBudgetMs = threading.webPumpBudgetMs;
    game.shaderCreation = settings.getShaderCreationSettings().engineSettings();
    game.atmosphereQuality = settings.getAtmosphereQuality();
    game.foliageQuality = settings.getFoliageQuality();
    game.foliageQualityUncapped = !settings.isFoliageQualityDeclared();
    game.foliageDistanceScale = settings.getFoliageDistanceScale();
    game.inputDeadzone = settings.getInputDeadzone();
    game.gamepadCursor = settings.getGamepadCursorSettings();
    game.simulateTouchFromMouse = settings.isSimulateTouchFromMouse();
    game.fixedTimestep = settings.getFixedTimestep();
    game.maximumDeltaTime = settings.getMaximumDeltaTime();
    game.inputActions = settings.getInputActionsEnginePath();
    game.navigation = settings.getNavigationSettings().settings;
    game.voxelMaxDimension = settings.getVoxelSettings().maxDimension;
    game.voxelCpuBudgetMB = settings.getVoxelSettings().cpuBudgetMB;
    game.voxelMeshBudgetMB = settings.getVoxelSettings().meshBudgetMB;
    const TextureProjectSettings& textures = settings.getTextureSettings();
    for (size_t g = 0; g < 3; g++)
        game.textureDefaults[g] = {textures.textureQuality[g], textures.memoryBudgetMB[g], textures.lowMemoryFallback[g]};
    game.htmlUiMsaa = settings.getHtmlUiMsaaSamples();
    game.glyphAtlasMiB = settings.getHtmlUiRenderSettings().glyphAtlasMiB;
    game.sdfPrimitives = settings.getHtmlUiRenderSettings().sdfPrimitives;
    game.transformAwareText = settings.getHtmlUiRenderSettings().transformAwareText;
    game.imageCacheMiB = settings.getHtmlUiRenderSettings().imageCacheMiB;
    game.mobilePbrTier = settings.getMobilePbrTier();
    game.dynamicBatching = settings.isDynamicBatching();
    game.gpuInstancing = settings.getDrawSubmissionSettings().gpuInstancing;
    game.lightMode = settings.getLightMode();
    game.transitions = settings.getTransitionDefaults();
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) game.shadowTiers[platform] = settings.getShadowTier(platform);
    game.device = settings.getDeviceSettings();
    for (int platform = 0; platform < terraincore::TerrainQualitySettings::PLATFORM_COUNT; platform++)
        game.terrainQualityTiers[platform] = static_cast<int>(settings.getTerrainQualityTier(platform));
    // the running export's (Project::beginTerrainExport), else all of them
    game.terrainShippedTiers = project.getGeneratedTerrainTiers();
    game.tags = settings.getTags();
    game.layerNames = settings.getLayerNames();
    if (settings.getSortingLayers().size() > 1) game.sortingLayers = settings.getSortingLayers();
    game.transparencySortMode = settings.getTransparencySortMode();
    game.transparencySortAxis = settings.getTransparencySortAxis();
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        game.collision3D[layer] = settings.getLayerCollisionMatrix3D()[layer];
        game.collision2D[layer] = settings.getLayerCollisionMatrix2D()[layer];
    }
    game.queriesHitTriggers3D = settings.getPhysicsQuerySettings().hitTriggers3D;
    game.queriesHitTriggers2D = settings.getPhysicsQuerySettings().hitTriggers2D;
    const Physics3DWorldSettings& world = settings.getPhysics3DWorldSettings();
    game.hasPhysics3DCapacity = true;
    game.maxBodies3D = world.maxBodies;
    game.maxBodyPairs3D = world.maxBodyPairs;
    game.maxContactConstraints3D = world.maxContactConstraints;
    game.shaderGlobals = settings.getShaderGlobals();

    const PlayerSettings player = settings.getPlayerSettings();
    game.hasPlayerSettings = true;
    game.targetFrameRate = player.targetFrameRate;
    game.defaultLanguage = player.defaultLanguage;
    game.defaultOrientation = player.defaultOrientation;
    game.allowedOrientations = player.allowedOrientations;
    game.keepScreenOn = player.keepScreenOn;
    game.pauseOnFocusLoss = player.pauseOnFocusLoss;
    // the project has String Tables (an export names what it shipped instead)
    if (player.localization) game.localization = "l10n/manifest.json";
    game.allowInsecureHttp = settings.getApplicationSettings().allowInsecureHttp;
    game.linuxApp = player.linuxApp;
    game.windowsApp = player.windowsApp;
    game.macosApp = player.macosApp;
    game.webApp = player.webApp;

    const PlayerLoggingSettings& logging = settings.getPlayerLogging();
    game.logLevel = logging.level;
    game.logFile = logging.fileLog;
    game.logBacktrace = logging.backtrace;
    const PlayerCrashSettings& crash = settings.getPlayerCrash();
    game.crashHandler = crash.enabled;
    game.crashLogTail = crash.includeLogTail;
    game.crashDumpWithHeap = crash.dumpWithHeap;
    game.hangWatchdog = crash.hangWatchdog;
    game.hangMs = crash.hangSeconds * 1000;
    game.crashWebOverlay = crash.webCrashOverlay;
    game.crashJournalBytes = crash.journalKiB * 1024;
    game.crashJournalEngineEvents = crash.journalEngineEvents;

    const DebugSettings& debug = settings.getDebugSettings();
    game.hasDebugSettings = true;
    game.debugOverlayMode = debug.overlay == DebugSettings::Mode::AllBuilds ? player::PlayerConfig::BuildMode::Always
                          : debug.overlay == DebugSettings::Mode::Development ? player::PlayerConfig::BuildMode::Development
                          : player::PlayerConfig::BuildMode::Never;
    game.debugAllowLuaMode = debug.allowLua == DebugSettings::AllowLua::AllBuilds ? player::PlayerConfig::BuildMode::Always
                           : debug.allowLua == DebugSettings::AllowLua::Development ? player::PlayerConfig::BuildMode::Development
                           : player::PlayerConfig::BuildMode::Never;
    game.debugOverlay = debug.toOverlaySettings(false);
    game.logBufferSize = debug.logBufferSize;
    game.logStackTraces = debug.logStackTraces;
    game.consoleCommands = DebugSettingsNames::name(debug.consoleCommands);
    // Player > Saved Data
    game.saveGame = settings.getSaveGameSettings().engineSettings();

    // Project Settings > Audio, as Play applies them (Project::setAudioSettings): the mixer by its
    // asset-relative path, which ships
    const AudioSettings& audio = settings.getAudioSettings();
    game.audioMixer = audio.mixer;
    game.audioMaxRealVoices = audio.maxRealVoices;
    game.audioMaxVoices = audio.maxVoices;
    game.audioSeed = audio.seed;
    game.audioUnusedClipCacheMb = audio.unusedClipCacheMb;
    game.audioMusicGroup = audio.musicGroup;
    game.audioSampleRate = audio.sampleRate;
    game.audioOutputStage = audio.outputStage;
    game.audioIosSession = audio.iosSessionCategory;
    return game;
}
