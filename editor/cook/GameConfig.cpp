// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/CppIdentifier.h"
#include "GameConfig.h"

#include "Project.h"
#include "render/shadow/ShadowTiers.h"
#include "terrain/TerrainQuality.h"
#include "util/NativeNeeds.h"
#include "util/NativePlugins.h"

using namespace jasperengine;
using namespace jasperengine::editor;

player::PlayerConfig editor::cook::makeGameConfig(Project& project, const GameConfigTarget& target) {
    player::PlayerConfig game;
    const WindowSettings window = project.getWindowSettings();
    game.name = project.getName().empty() ? target.fallbackName : project.getName();
    if (target.platform) {
        const BuildPlatform platform = *target.platform;
        game.appId = platform == BuildPlatform::Android ? project.getAndroidProjectSettings().packageName
                   : platform == BuildPlatform::MacOS ? project.getMacOSProjectSettings().bundleIdentifier
                   : platform == BuildPlatform::IOS ? project.getIOSProjectSettings().bundleIdentifier
                   : target.fallbackName;
    }
    if (game.appId.empty()) game.appId = target.fallbackName;
    game.version = project.getAndroidProjectSettings().versionName;
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
    game.vsync = project.isVSyncEnabled();
    game.storageId = toIdentifier(game.name);
    game.canvasWidth = project.getCanvasWidth();
    game.canvasHeight = project.getCanvasHeight();
    game.scaling = project.getScalingMode();
    game.pixelsPerUnit = project.getPixelsPerUnit();
    game.defaultFilterPoint = project.getDefaultFilterMode() == TextureFilterMode::Point;
    game.bitmapTextCacheMiB = project.getBitmapTextCacheMiB();
    game.textureStrategy = project.getTextureStrategy();
    game.canvasScaler = project.getUIScale();
    const LoadingSettings& loading = project.getLoadingSettings();
    game.loadingScene = loading.sceneId;
    game.loadingDelay = loading.delay;
    game.loadingTimeout = loading.timeout;
    game.asyncLoading = loading.asyncLoading;
    game.particleWorldBudget = project.getParticleWorldBudget();
    const ThreadingProjectSettings& threading = project.getThreadingSettings();
    game.threading.workers = threading.workers;
    game.threading.reservedFrameWorkers = threading.reservedFrameWorkers;
    game.threading.ioThreads = threading.ioThreads;
    game.threading.webPumpBudgetMs = threading.webPumpBudgetMs;
    game.shaderCreation = project.getShaderCreationSettings().engineSettings();
    game.atmosphereQuality = project.getAtmosphereQuality();
    game.foliageQuality = project.getFoliageQuality();
    game.foliageQualityUncapped = !project.isFoliageQualityDeclared();
    game.foliageDistanceScale = project.getFoliageDistanceScale();
    game.inputDeadzone = project.getInputDeadzone();
    game.inputActions = project.getInputActionsEnginePath();
    game.navigation = project.getNavigationSettings().settings;
    game.voxelMaxDimension = project.getVoxelSettings().maxDimension;
    game.voxelCpuBudgetMB = project.getVoxelSettings().cpuBudgetMB;
    game.voxelMeshBudgetMB = project.getVoxelSettings().meshBudgetMB;
    const TextureProjectSettings& textures = project.getTextureSettings();
    for (size_t g = 0; g < 3; g++)
        game.textureDefaults[g] = {textures.textureQuality[g], textures.memoryBudgetMB[g], textures.lowMemoryFallback[g]};
    game.htmlUiMsaa = project.getHtmlUiMsaaSamples();
    game.glyphAtlasMiB = project.getHtmlUiRenderSettings().glyphAtlasMiB;
    game.sdfPrimitives = project.getHtmlUiRenderSettings().sdfPrimitives;
    game.transformAwareText = project.getHtmlUiRenderSettings().transformAwareText;
    game.mobilePbrTier = project.getMobilePbrTier();
    game.dynamicBatching = project.isDynamicBatching();
    game.gpuInstancing = project.getDrawSubmissionSettings().gpuInstancing;
    game.lightMode = project.getLightMode();
    game.transitions = project.getTransitionDefaults();
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) game.shadowTiers[platform] = ShadowTiers::getPlatformTier(platform);
    game.device = project.getDeviceSettings();
    for (int platform = 0; platform < terraincore::TerrainQualitySettings::PLATFORM_COUNT; platform++)
        game.terrainQualityTiers[platform] = static_cast<int>(project.getTerrainQualityTier(platform));
    // the running export's (Project::beginTerrainExport), else all of them
    game.terrainShippedTiers = project.getGeneratedTerrainTiers();
    game.tags = project.getTags();
    game.layerNames = project.getLayerNames();
    if (project.getSortingLayers().size() > 1) game.sortingLayers = project.getSortingLayers();
    game.transparencySortMode = project.getTransparencySortMode();
    game.transparencySortAxis = project.getTransparencySortAxis();
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        game.collision3D[layer] = project.getLayerCollisionMatrix3D()[layer];
        game.collision2D[layer] = project.getLayerCollisionMatrix2D()[layer];
    }
    const Physics3DWorldSettings& world = project.getPhysics3DWorldSettings();
    game.hasPhysics3DCapacity = true;
    game.maxBodies3D = world.maxBodies;
    game.maxBodyPairs3D = world.maxBodyPairs;
    game.maxContactConstraints3D = world.maxContactConstraints;
    game.shaderGlobals = project.getShaderGlobals();

    const PlayerSettings player = project.getPlayerSettings();
    game.hasPlayerSettings = true;
    game.targetFrameRate = player.targetFrameRate;
    game.defaultLanguage = player.defaultLanguage;
    game.defaultOrientation = player.defaultOrientation;
    game.allowedOrientations = player.allowedOrientations;
    game.keepScreenOn = player.keepScreenOn;
    game.allowInsecureHttp = project.getApplicationSettings().allowInsecureHttp;
    game.linuxApp = player.linuxApp;
    game.windowsApp = player.windowsApp;
    game.macosApp = player.macosApp;
    game.webApp = player.webApp;

    const PlayerLoggingSettings& logging = project.getPlayerLogging();
    game.logLevel = logging.level;
    game.logFile = logging.fileLog;
    game.logBacktrace = logging.backtrace;
    const PlayerCrashSettings& crash = project.getPlayerCrash();
    game.crashHandler = crash.enabled;
    game.crashLogTail = crash.includeLogTail;
    game.crashDumpWithHeap = crash.dumpWithHeap;
    game.hangWatchdog = crash.hangWatchdog;
    game.hangMs = crash.hangSeconds * 1000;
    game.crashWebOverlay = crash.webCrashOverlay;

    const DebugSettings& debug = project.getDebugSettings();
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

    // Project Settings > Audio, as Play applies them (Project::setAudioSettings): the mixer by its
    // asset-relative path, which ships
    const AudioSettings& audio = project.getAudioSettings();
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
