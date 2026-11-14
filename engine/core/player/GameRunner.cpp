// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GameRunner.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "Screen.h"
#include "System.h"
#include "debug/DebugOverlay.h"
#include "device/Device.h"
#include "input/InputActions.h"
#include "io/Data.h"
#include "io/StrippedAssets.h"
#include "manager/PrefabManager.h"
#include "manager/SceneManager.h"
#include "native/NativeBridge.h"
#include "navigation/NavTypes.h"
#include "net/Http.h"
#include "particles/ParticleWorld.h"
#include "pixelart/BitmapTextCache.h"
#include "pixelart/ScreenTransition.h"
#include "player/ContentManifest.h"
#include "player/RunControl.h"
#include "registry/PhysicsLayers.h"
#include "registry/SortingLayers.h"
#include "registry/TagManager.h"
#include "render/LightingTiers.h"
#include "render/shadow/ShadowTiers.h"
#include "script/LuaBinding.h"
#include "serialization/CookedScene.h"
#include "shader/ShaderGlobals.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "terrain/TerrainQuality.h"
#include "texture/MaterialPbr.h"
#include "util/TitleArtLanguage.h"
#if JASPERENGINE_VOXEL
#include "subsystem/VoxelSystem.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    // what runGame was given: the stacks' factories read it when SceneManager runs them
    struct RunningGame {
        PlayerConfig game;
        ProjectHooks hooks;
        std::string contentUrl;
        // the scenes made so far, by id (one Scene per id, as SceneManager knows them)
        std::map<uint32_t, Scene*> slots;
    };

    RunningGame& running() {
        static RunningGame state;
        return state;
    }

    void initSceneScripts(Scene* scene) {
        if (running().hooks.initScripts) running().hooks.initScripts(scene);
        else LuaBinding::initializeLuaScripts(scene);
    }

    void cleanupSceneScripts(Scene* scene) {
        if (running().hooks.cleanupScripts) running().hooks.cleanupScripts(scene);
        else LuaBinding::cleanupLuaScripts(scene);
    }

    void destroyScene(uint32_t id) {
        Scene*& scene = running().slots[id];
        if (!scene) return;
        cleanupSceneScripts(scene);
        SceneManager::removeScenePtr(id);
        delete scene;
        scene = nullptr;
    }

    // create the scenes, register their pointers so cross-scene references resolve, then build
    // and start the new ones
    void setupStack(const GameScene& stack) {
        RunningGame& state = running();
        std::vector<uint32_t> made;
        for (uint32_t id : stack.involved) {
            Scene*& scene = state.slots[id];
            if (!scene) {
                scene = new Scene();
                SceneManager::setScenePtr(id, scene);
                made.push_back(id);
            }
        }
        for (uint32_t id : made) {
            const GameScene* info = state.game.findScene(id);
            std::string error;
            if (!info || !CookedSceneLoader::loadScene(state.slots[id], state.contentUrl + info->file, &error))
                Log::error("Scene %u: %s", id, info ? error.c_str() : "not in game.json");
        }
        for (uint32_t id : made) initSceneScripts(state.slots[id]);
    }

    void loadStack(const GameScene& stack) {
        RunningGame& state = running();
        // a loaded stack starts again, except scenes still on screen (the loading scene)
        for (uint32_t id : stack.involved) {
            Scene* scene = state.slots[id];
            if (scene && !Engine::isSceneRunning(scene)) destroyScene(id);
        }
        setupStack(stack);
        for (uint32_t id : stack.active) {
            Scene* scene = state.slots[id];
            if (!scene) continue;
            if (id == stack.id) Engine::setScene(scene);
            else Engine::addSceneLayer(scene);
        }
        // The scenes no stack on screen involves go. A script started above may have added a
        // stack of its own (SceneManager::addChildScene): a scene is kept while any stack that
        // involves it is on screen, as deleting it would leave a freed Scene behind
        for (const GameScene& other : state.game.scenes) {
            if (other.id == stack.id) continue;
            if (std::find(stack.involved.begin(), stack.involved.end(), other.id) != stack.involved.end()) continue;
            Scene* scene = state.slots[other.id];
            if (!scene) continue;
            bool kept = false;
            for (const GameScene& owner : state.game.scenes) {
                if (std::find(owner.involved.begin(), owner.involved.end(), other.id) == owner.involved.end()) continue;
                Scene* ownerScene = state.slots[owner.id];
                if (ownerScene && Engine::isSceneRunning(ownerScene)) kept = true;
            }
            if (!kept) destroyScene(other.id);
        }
    }

    // a prefab's cooked bytes, read once
    std::shared_ptr<const std::vector<uint8_t>> prefabBytes(const std::string& path) {
        static std::mutex mutex;
        static std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> cache;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        Data data;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (data.open(path.c_str()) == FileErrors::FILEDATA_OK) bytes->assign(data.getMemPtr(), data.getMemPtr() + data.length());
        cache[path] = bytes;
        return bytes;
    }

    // the native plugins this build cannot run: a prebuilt build's are its platform's, a source
    // export's are named per target (the Xcode project builds both Apple apps)
    const std::map<std::string, std::string>* platformOnlyPlugins(const PlayerConfig& game) {
        if (!game.nativePlatformOnly.empty()) return &game.nativePlatformOnly;
#if defined(__ANDROID__)
        const char* target = "android";
#elif defined(__EMSCRIPTEN__)
        const char* target = "web";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
        const char* target = "ios";
#elif defined(__APPLE__) && defined(JASPERENGINE_MACOS_CMAKE)
        const char* target = "macos-cmake";
#elif defined(__APPLE__)
        const char* target = "macos";
#else
        const char* target = "desktop";
#endif
        auto it = game.nativePlatformOnlyByTarget.find(target);
        return it != game.nativePlatformOnlyByTarget.end() && !it->second.empty() ? &it->second : nullptr;
    }

    bool resolveBuildMode(PlayerConfig::BuildMode mode) {
        return mode == PlayerConfig::BuildMode::Always || (mode == PlayerConfig::BuildMode::Development && Engine::isDevelopmentBuild());
    }

}

bool player::readGameConfig(const std::string& contentUrl, PlayerConfig& out, std::string& error) {
    Data data;
    const std::string path = contentUrl + "game.json";
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
        error = "no game.json at " + path + ": build the game again (Build Settings > Build)";
        return false;
    }
    const std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    if (!PlayerConfig::parse(text, out, error)) return false;
    // a Compile build's Development Build names the setting at a load of an asset it left out
    // (S98 REQ-10; the prebuilt player reads its manifest itself); the IDE's build has no manifest
    Data manifestData;
    if (manifestData.open((contentUrl + "content-manifest.json").c_str()) == FileErrors::FILEDATA_OK) {
        ContentManifest manifest;
        std::string manifestError;
        const std::string manifestText(reinterpret_cast<const char*>(manifestData.getMemPtr()), manifestData.length());
        if (ContentManifest::parse(manifestText, manifest, manifestError)) StrippedAssets::set(manifest.stripped);
        else Log::warn("content-manifest.json: %s", manifestError.c_str());
    }
    return true;
}

void player::applySettings(const PlayerConfig& game) {
    // Threading first: the job system and the asset readers start with these counts
    const ThreadingSettings threadingDefaults;
    if (game.threading.workers != threadingDefaults.workers || game.threading.reservedFrameWorkers != threadingDefaults.reservedFrameWorkers ||
        game.threading.ioThreads != threadingDefaults.ioThreads || game.threading.webPumpBudgetMs != threadingDefaults.webPumpBudgetMs)
        Engine::setThreadingSettings(game.threading);
    // applied when the view loads, which comes after this
    if (game.shaderCreation != ShaderCreationSettings()) Engine::setShaderCreationSettings(game.shaderCreation);
    Engine::setCanvasSize(game.canvasWidth, game.canvasHeight);
    Engine::setScalingMode(game.scaling);
    Engine::setPixelsPerUnit(game.pixelsPerUnit);
    if (game.defaultFilterPoint) Engine::setDefaultTextureFilter(TextureFilter::NEAREST);
    pixelart::BitmapTextCache::get().setBudgetMiB(game.bitmapTextCacheMiB);
    if (game.canvasScaler != CanvasScalerSettings()) Engine::setDefaultCanvasScaler(game.canvasScaler);
    Engine::setTextureStrategy(game.textureStrategy);
    Engine::setAtmosphereQualityDefaults(game.atmosphereQuality[0], game.atmosphereQuality[1], game.atmosphereQuality[2]);
    Engine::setFoliageQualityDefaults(game.foliageQuality[0], game.foliageQuality[1], game.foliageQuality[2], game.foliageQualityUncapped);
    Engine::setFoliageDistanceScaleDefaults(game.foliageDistanceScale[0], game.foliageDistanceScale[1], game.foliageDistanceScale[2]);
    if (game.inputDeadzone != 0.15f) InputActions::setDefaultDeadzone(game.inputDeadzone);
    if (!game.inputActions.empty()) InputActions::loadDefault(game.inputActions);
    if (game.navigation != nav::NavigationSettings()) nav::setSettings(game.navigation);
#if JASPERENGINE_VOXEL
    if (game.voxelMaxDimension > 0 || game.voxelCpuBudgetMB > 0 || game.voxelMeshBudgetMB > 0) {
        constexpr int64_t mebibyte = 1024 * 1024;
        VoxelLimits limits = voxelPlatformLimits();
        if (game.voxelMaxDimension > 0) limits.maxDimension = std::min(limits.maxDimension, game.voxelMaxDimension);
        if (game.voxelCpuBudgetMB > 0) limits.cpuBudget = std::min(limits.cpuBudget, game.voxelCpuBudgetMB * mebibyte);
        if (game.voxelMeshBudgetMB > 0) limits.meshBudget = std::min(limits.meshBudget, game.voxelMeshBudgetMB * mebibyte);
        setVoxelLimitsOverride(limits);
    }
#endif
    Engine::setTextureDefaults(game.textureDefaults[0], game.textureDefaults[1], game.textureDefaults[2]);
    // the Device Tier the GPU gets applies from here on (spec S56)
    Device::configure(game.device);
    UIDocumentSystem::setPreferredSampleCount(game.htmlUiMsaa);
    UIDocumentSystem::setHtmlUiRenderOptions(game.glyphAtlasMiB, game.sdfPrimitives, game.transformAwareText);
    if (game.mobilePbrTier != PbrTier::REDUCED) MaterialPbr::setMobileTier(game.mobilePbrTier);
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++)
        if (game.shadowTiers[platform] != ShadowTiers::defaultTier(platform)) ShadowTiers::setPlatformTier(platform, game.shadowTiers[platform]);
    if (!game.gpuInstancing) Engine::setGpuInstancingEnabled(false);
    // Screen Transitions (S48): Lua's defaults
    pixelart::ScreenTransition::setDefaults(game.transitions);
    if (game.lightMode != LightMode::AUTOMATIC) LightingTiers::setLightMode(game.lightMode);
    // T3 R2, R34: the terrain tiers of the platforms, and the ones the build shipped
    for (int platform = 0; platform < terraincore::TerrainQualitySettings::PLATFORM_COUNT; platform++) {
        const auto tier = static_cast<terraincore::TerrainQuality>(std::clamp(game.terrainQualityTiers[platform], 0, 2));
        if (tier != terraincore::TerrainQuality::HIGH) terraincore::TerrainQualitySettings::setPlatformTier(platform, tier);
    }
    if (game.terrainShippedTiers != terraincore::TERRAIN_ALL_TIERS) terraincore::TerrainQualitySettings::setShippedMask(game.terrainShippedTiers);
    // native plugins with code for other platforms only: "unavailable" here, and a Lua mock
    // standing in for one says so once
    if (const std::map<std::string, std::string>* only = platformOnlyPlugins(game)) {
        NativeBridge::setPlatformOnlyPlugins(*only);
        NativeBridge::setMockWarnings(true);
    }
    // Player > Resolution and Presentation (before any script, which may change them) and what
    // System.getAppName... return where there is no package to ask
    Engine::setVSyncCount(game.vsync ? 1 : 0);
    Engine::setDynamicBatchingDefault(game.dynamicBatching);
    if (game.hasPlayerSettings) {
        Http::setInsecureHttpAllowed(game.allowInsecureHttp);
        Engine::setTargetFrameRate(game.targetFrameRate);
        // Project Settings > Localization > Default Language (S87 R12 step 4)
        TitleArtLanguage::setProjectDefault(game.defaultLanguage);
        Screen::initialize(game.defaultOrientation, game.allowedOrientations, game.keepScreenOn);
#if defined(_WIN32)
        System::setApplicationInfo(game.windowsApp);
#elif defined(__EMSCRIPTEN__)
        System::setApplicationInfo(game.webApp);
#elif defined(__APPLE__)
        System::setApplicationInfo(game.macosApp);
#else
        System::setApplicationInfo(game.linuxApp);
#endif
    }
    // Tags and Layers, before any scene or script can ask for them
    if (!game.tags.empty()) TagManager::setTags(game.tags);
    for (size_t layer = 0; layer < game.layerNames.size(); layer++)
        if (!game.layerNames[layer].empty()) TagManager::setLayerName(static_cast<int>(layer), game.layerNames[layer]);
    if (!game.sortingLayers.empty()) SortingLayers::setLayers(game.sortingLayers);
    SortingLayers::setTransparencySortMode(game.transparencySortMode);
    SortingLayers::setTransparencySortAxis(game.transparencySortAxis);
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        if (game.collision3D[layer] != LAYER_MASK_EVERYTHING) Physics::setLayerCollisionMask(layer, game.collision3D[layer]);
        if (game.collision2D[layer] != LAYER_MASK_EVERYTHING) Physics2D::setLayerCollisionMask(layer, game.collision2D[layer]);
    }
#ifdef JASPERENGINE_PHYSICS_3D
    // before any scene (and its world) is made
    if (game.hasPhysics3DCapacity) PhysicsSystem::setWorldCapacity3D(game.maxBodies3D, game.maxBodyPairs3D, game.maxContactConstraints3D);
#endif
    if (game.particleWorldBudget != 0) particles::ParticleWorld::setProjectBudget(game.particleWorldBudget);
    // the voices and the project's mixer, before the first scene
    applyAudio(game);
    // declared with their defaults before any scene draws
    if (!game.shaderGlobals.empty()) ShaderGlobals::declare(game.shaderGlobals);
    // Project Settings > Debug, before the first scene (its scripts may read DebugOverlay)
#if JASPERENGINE_DEVELOPMENT_BUILD || defined(DEBUG)
    Engine::setDevelopmentBuild(true);
#endif
    if (game.hasDebugSettings) {
        Log::setBufferSize(game.logBufferSize);
        Log::setStackTraces(game.logStackTraces);
        DebugOverlaySettings overlay = game.debugOverlay;
        overlay.enabled = resolveBuildMode(game.debugOverlayMode);
        overlay.allowLua = resolveBuildMode(game.debugAllowLuaMode);
        DebugOverlay::configure(overlay);
    }
}

void player::runGame(const PlayerConfig& game, const ProjectHooks& hooks, const std::string& contentUrl) {
    RunningGame& state = running();
    state.game = game;
    state.hooks = hooks;
    state.contentUrl = contentUrl;

    applySettings(game);

    for (const GameScene& scene : game.scenes) {
        const GameScene stack = scene;
        SceneManager::registerScene(stack.id, stack.name, [stack]() { loadStack(stack); }, [stack]() { setupStack(stack); }, stack.active,
                                    [path = contentUrl + stack.file]() { return CookedSceneLoader::sceneResources(path); });
    }
    for (const GamePrefab& prefab : game.prefabs) {
        const std::string path = contentUrl + prefab.file;
        PrefabManager::registerPrefab(prefab.id, prefab.name, prefab.guid, [path](Scene* scene, Entity root) {
            const auto bytes = prefabBytes(path);
            std::string error;
            if (bytes->empty() || !CookedSceneLoader::instantiatePrefab(scene, root, *bytes, nullptr, &error)) {
                Log::error("Prefab %s: %s", path.c_str(), bytes->empty() ? "cannot be read" : error.c_str());
                return false;
            }
            return true;
        });
    }
    // the C++ scripts of the GameObjects scripts instantiate, destroy and move to another scene
    if (hooks.initEntityScripts || hooks.cleanupEntityScripts) ObjectManager::setCppScriptHooks(hooks.initEntityScripts, hooks.cleanupEntityScripts);
    if (hooks.moveEntityScripts) ObjectManager::setCppScriptMoveHook(hooks.moveEntityScripts);

    if (game.asyncLoading) Engine::setAsyncLoading(true);
    if (game.loadingScene != 0 && game.findScene(game.loadingScene)) SceneManager::setLoadingScene(game.loadingScene);
    if (game.loadingDelay != 0.0f) SceneManager::setLoadingDelay(game.loadingDelay);
    if (game.loadingTimeout != 5.0f) SceneManager::setLoadingTimeout(game.loadingTimeout);

    const std::string& requested = RunControl::options().scene;
    const GameScene* start = requested.empty() ? game.findScene(game.startScene) : game.findScene(requested);
    if (!start && !game.scenes.empty()) start = &game.scenes.front();
    if (start) SceneManager::loadScene(start->id);
}
