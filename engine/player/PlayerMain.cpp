// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The prebuilt player (docs/specs/S30-shipping.md R1, D3): one executable per platform, backend
// and flavour that runs any game's cooked content. Before the window exists it finds its content
// (next to the executable, the bundle's resources, --content), checks content-manifest.json
// (exit 5 on a mismatch) and reads game.json into the platform configuration; its init() then
// does what a Compile build's generated init() does, with the scenes and prefabs read from their
// cooked files instead of compiled in.

#include "JasperEngine.h"

#include "PlatformConfig.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "player/ContentManifest.h"
#include "player/Launch.h"
#include "player/PlayerConfig.h"
#include "player/RunControl.h"
#include "serialization/CookedScene.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    PlayerConfig game;

    // where a player looks for its content when --content does not say (R2)
    std::string defaultContentRoot() {
#if defined(__ANDROID__)
        // the APK's assets/game/ (the APK's assets root keeps no other tree)
        return "game";
#elif defined(__EMSCRIPTEN__)
        // the fetched content.jpak, mounted before main
        return "";
#elif defined(__APPLE__)
        std::string root;
        if (CFBundleRef bundle = CFBundleGetMainBundle()) {
            if (CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle)) {
                if (CFURLRef absolute = CFURLCopyAbsoluteURL(url)) {
                    char path[4096];
                    if (CFURLGetFileSystemRepresentation(absolute, true, reinterpret_cast<UInt8*>(path), sizeof(path))) root = path;
                    CFRelease(absolute);
                }
                CFRelease(url);
            }
        }
    #if TARGET_OS_IPHONE
        // the iOS template stages the content into Content/ of the app
        return root.empty() ? "Content" : root + "/Content";
    #else
        // a .app keeps it in Contents/Resources; a bare executable next to itself
        if (!root.empty() && std::filesystem::exists(std::filesystem::path(root) / "game.json")) return root;
        char executable[4096];
        uint32_t size = sizeof(executable);
        if (_NSGetExecutablePath(executable, &size) == 0)
            return std::filesystem::weakly_canonical(executable).parent_path().string();
        return root.empty() ? "." : root;
    #endif
#elif defined(_WIN32)
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) return ".";
        return std::filesystem::path(std::wstring(path, length)).parent_path().u8string();
#else
        char path[4096];
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0) return ".";
        path[length] = '\0';
        return std::filesystem::path(path).parent_path().string();
#endif
    }

    bool readContent(const std::string& relative, std::string& out) {
        Data data;
        if (data.open(("content://" + relative).c_str()) != FileErrors::FILEDATA_OK) return false;
        out.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    bool contentExists(const std::string& relative) {
        if (ResourcePack::contains("content://" + relative)) return true;
        // the web's pack was fetched by the page and mounted from memory
        if (relative == "content.jpak" && ResourcePack::contentPack() && platformConfig().contentRoot.empty()) return true;
        FILE* file = System::hasInstance() ? System::instance().platformFopen(FileData::getSystemPath("content://" + relative).c_str(), "rb")
                                           : std::fopen((platformConfig().contentRoot + "/" + relative).c_str(), "rb");
        if (!file) return false;
        std::fclose(file);
        return true;
    }

    bool contentSha256(const std::string& relative, std::string& hex, uint64_t& size) {
        std::string bytes;
        if (!readContent(relative, bytes)) return false;
        hex = sha256Hex(bytes);
        size = bytes.size();
        return true;
    }

    // a system is needed for content:// before the platform made its own: the paths of the
    // content root, the same the platform's will report
    class ContentSystem : public System {
    public:
        int getScreenWidth() override { return platformConfig().width; }
        int getScreenHeight() override { return platformConfig().height; }
        std::string getContentPath() override { return platformConfig().contentRoot; }
        std::string getAssetPath() override {
            return platformConfig().contentRoot.empty() ? std::string() : platformConfig().contentRoot + "/assets";
        }
    };

    // what a player tells before it gives up: stderr, and logcat on Android (no stderr there)
    void report(const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "JasperEngine", "%s", message.c_str());
#endif
    }

    int failContent(const std::string& message) {
        report("Content check failed: " + message);
        return EXIT_CONTENT_MISMATCH;
    }

    // the player's step before the window (Launch.h)
    int configure(const LaunchOptions& options) {
        PlatformConfig& platform = platformConfig();
        if (platform.contentRoot.empty()) platform.contentRoot = defaultContentRoot();

        // the platform's System comes with its window; until then the content root's
        ContentSystem early;
        EarlySystemScope earlyScope(&early);

        // a packed build's content.jpak (the web mounted its fetched one already)
        if (!ResourcePack::contentPack()) {
            const std::filesystem::path pack = std::filesystem::path(platform.contentRoot) / "content.jpak";
            std::error_code ec;
            if (std::filesystem::is_regular_file(pack, ec)) {
                std::string error;
                if (!ResourcePack::mountContentPack(pack, &error)) return failContent("content.jpak: " + error);
            }
        }

        std::string text;
        if (!readContent("content-manifest.json", text))
            return failContent("no content-manifest.json in " + (platform.contentRoot.empty() ? std::string("the content pack") : platform.contentRoot) +
                               ": run a game built for this player (Build Settings > Build, or jasperengine-editor build), or give "
                               "--content <folder of the build>");
        ContentManifest manifest;
        std::string error;
        if (!ContentManifest::parse(text, manifest, error)) return failContent(error);
        std::vector<std::string> problems;
        const ContentAccess access{&contentExists, &contentSha256};
        if (!checkContent(manifest, runtimePlayerId(), access, options.verifyContent, problems)) {
            for (const std::string& problem : problems) report("Content check failed: " + problem);
            return EXIT_CONTENT_MISMATCH;
        }

        if (!readContent("game.json", text)) return failContent("game.json is missing");
        if (!PlayerConfig::parse(text, game, error)) return failContent(error);
        if (!options.scene.empty() && !game.findScene(options.scene)) {
            std::fprintf(stderr, "--scene %s: no scene of that name or id; the game has", options.scene.c_str());
            for (const GameScene& scene : game.scenes) std::fprintf(stderr, " \"%s\" (%u)", scene.name.c_str(), scene.id);
            std::fprintf(stderr, "\n");
            return EXIT_BAD_ARGUMENTS;
        }

        platform.windowMode = game.windowMode;
        platform.width = game.windowWidth;
        platform.height = game.windowHeight;
        platform.resizable = game.resizable;
        platform.vsync = game.vsync;
        platform.title = game.title;
        platform.appId = game.appId.empty() ? game.name : game.appId;
        platform.storageId = game.storageId;
        if (!game.icon.empty() && contentExists(game.icon) && !platform.contentRoot.empty())
            platform.iconPath = platform.contentRoot + "/" + game.icon;
        platform.logLevel = game.logLevel;
        platform.logFile = game.logFile ? 1 : 0;
        platform.logBacktrace = game.logBacktrace ? 1 : 0;
        platform.crashHandler = game.crashHandler ? 1 : 0;
        platform.crashLogTail = game.crashLogTail ? 1 : 0;
        platform.crashDumpWithHeap = game.crashDumpWithHeap ? 1 : 0;
        platform.hangWatchdog = game.hangWatchdog ? 1 : 0;
        platform.hangMs = game.hangMs;
        platform.crashWebOverlay = game.crashWebOverlay ? 1 : 0;
        return -1;
    }

    struct ConfiguratorRegistrar {
        ConfiguratorRegistrar() { setGameConfigurator(&configure); }
    } configuratorRegistrar;

    // ---------------------------------------------------------------- scene stacks

    // the scenes made so far, by id (a Compile build's static Scene* per scene)
    std::map<uint32_t, Scene*>& sceneSlots() {
        static std::map<uint32_t, Scene*> slots;
        return slots;
    }

    void destroyScene(uint32_t id) {
        Scene*& scene = sceneSlots()[id];
        if (!scene) return;
        LuaBinding::cleanupLuaScripts(scene);
        SceneManager::removeScenePtr(id);
        delete scene;
        scene = nullptr;
    }

    // create the scenes, register their pointers so cross-scene references resolve, then build
    // and start the new ones (Generator::configure's stack setup)
    void setupStack(const GameScene& stack) {
        std::vector<uint32_t> made;
        for (uint32_t id : stack.involved) {
            Scene*& scene = sceneSlots()[id];
            if (!scene) {
                scene = new Scene();
                SceneManager::setScenePtr(id, scene);
                made.push_back(id);
            }
        }
        for (uint32_t id : made) {
            const GameScene* info = game.findScene(id);
            std::string error;
            if (!info || !CookedSceneLoader::loadScene(sceneSlots()[id], "content://" + info->file, &error))
                Log::error("Scene %u: %s", id, info ? error.c_str() : "not in game.json");
        }
        for (uint32_t id : made) LuaBinding::initializeLuaScripts(sceneSlots()[id]);
    }

    void loadStack(const GameScene& stack) {
        // a loaded stack starts again, except scenes still on screen (the loading scene)
        for (uint32_t id : stack.involved) {
            Scene* scene = sceneSlots()[id];
            if (scene && !Engine::isSceneRunning(scene)) destroyScene(id);
        }
        setupStack(stack);
        for (uint32_t id : stack.active) {
            Scene* scene = sceneSlots()[id];
            if (!scene) continue;
            if (id == stack.id) Engine::setScene(scene);
            else Engine::addSceneLayer(scene);
        }
        // the scenes no stack on screen involves go
        for (const GameScene& other : game.scenes) {
            if (other.id == stack.id) continue;
            if (std::find(stack.involved.begin(), stack.involved.end(), other.id) != stack.involved.end()) continue;
            Scene* scene = sceneSlots()[other.id];
            if (!scene) continue;
            bool kept = false;
            for (const GameScene& owner : game.scenes) {
                if (std::find(owner.involved.begin(), owner.involved.end(), other.id) == owner.involved.end()) continue;
                Scene* ownerScene = sceneSlots()[owner.id];
                if (ownerScene && Engine::isSceneRunning(ownerScene)) kept = true;
            }
            if (!kept) destroyScene(other.id);
        }
    }

    // a prefab's cooked bytes, read once
    std::shared_ptr<const std::vector<uint8_t>> prefabBytes(const std::string& file) {
        static std::mutex mutex;
        static std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> cache;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(file);
        if (it != cache.end()) return it->second;
        Data data;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (data.open(("content://" + file).c_str()) == FileErrors::FILEDATA_OK)
            bytes->assign(data.getMemPtr(), data.getMemPtr() + data.length());
        cache[file] = bytes;
        return bytes;
    }

}

JASPERENGINE_INIT void init() {
    const ThreadingSettings threadingDefaults;
    if (game.threading.workers != threadingDefaults.workers || game.threading.reservedFrameWorkers != threadingDefaults.reservedFrameWorkers ||
        game.threading.ioThreads != threadingDefaults.ioThreads || game.threading.webPumpBudgetMs != threadingDefaults.webPumpBudgetMs)
        Engine::setThreadingSettings(game.threading);
    Engine::setCanvasSize(game.canvasWidth, game.canvasHeight);
    Engine::setScalingMode(game.scaling);
    Engine::setPixelsPerUnit(game.pixelsPerUnit);
    if (game.defaultFilterPoint) Engine::setDefaultTextureFilter(TextureFilter::NEAREST);
    Engine::setTextureStrategy(game.textureStrategy);
    Engine::setAtmosphereQualityDefaults(game.atmosphereQuality[0], game.atmosphereQuality[1], game.atmosphereQuality[2]);
    Engine::setFoliageDistanceScaleDefaults(game.foliageDistanceScale[0], game.foliageDistanceScale[1], game.foliageDistanceScale[2]);
    if (game.inputDeadzone != 0.15f) InputActions::setDefaultDeadzone(game.inputDeadzone);
    if (!game.inputActions.empty()) InputActions::loadDefault(game.inputActions);
    if (game.navigation != nav::NavigationSettings()) nav::setSettings(game.navigation);
    Engine::setTextureDefaults(game.textureDefaults[0], game.textureDefaults[1], game.textureDefaults[2]);
    UIDocumentSystem::setPreferredSampleCount(game.htmlUiMsaa);
    UIDocumentSystem::setHtmlUiRenderOptions(game.glyphAtlasMiB, game.sdfPrimitives, game.transformAwareText);
    if (game.mobilePbrTier != PbrTier::REDUCED) MaterialPbr::setMobileTier(game.mobilePbrTier);
    Engine::setDynamicBatchingDefault(game.dynamicBatching);
    if (game.lightMode != LightMode::AUTOMATIC) LightingTiers::setLightMode(game.lightMode);
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++)
        if (game.shadowTiers[platform] != ShadowTiers::defaultTier(platform)) ShadowTiers::setPlatformTier(platform, game.shadowTiers[platform]);
    // the Device Tier the GPU gets applies from here on (spec S56)
    Device::configure(game.device);
    if (!game.tags.empty()) TagManager::setTags(game.tags);
    for (size_t layer = 0; layer < game.layerNames.size(); layer++)
        if (!game.layerNames[layer].empty()) TagManager::setLayerName(static_cast<int>(layer), game.layerNames[layer]);
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        if (game.collision3D[layer] != LAYER_MASK_EVERYTHING) Physics::setLayerCollisionMask(layer, game.collision3D[layer]);
        if (game.collision2D[layer] != LAYER_MASK_EVERYTHING) Physics2D::setLayerCollisionMask(layer, game.collision2D[layer]);
    }
    // the voices and the project's mixer, before the first scene (as a Compile build's init)
    player::applyAudio(game);
    // native plugins with code for other platforms only: "unavailable" here, and a Lua mock
    // standing in for one says so once (as a Compile build's init)
    if (!game.nativePlatformOnly.empty()) {
        NativeBridge::setPlatformOnlyPlugins(game.nativePlatformOnly);
        NativeBridge::setMockWarnings(true);
    }

    for (const GameScene& scene : game.scenes) {
        const GameScene stack = scene;
        SceneManager::registerScene(stack.id, stack.name, [stack]() { loadStack(stack); }, [stack]() { setupStack(stack); }, stack.active,
                                    [file = stack.file]() { return CookedSceneLoader::sceneResources("content://" + file); });
    }
    for (const GamePrefab& prefab : game.prefabs) {
        const std::string file = prefab.file;
        PrefabManager::registerPrefab(prefab.id, prefab.name, prefab.guid, [file](Scene* scene, Entity root) {
            const auto bytes = prefabBytes(file);
            std::string error;
            if (bytes->empty() || !CookedSceneLoader::instantiatePrefab(scene, root, *bytes, nullptr, &error)) {
                Log::error("Prefab %s: %s", file.c_str(), bytes->empty() ? "cannot be read" : error.c_str());
                return false;
            }
            return true;
        });
    }

    if (game.asyncLoading) Engine::setAsyncLoading(true);
    if (game.loadingScene != 0 && game.findScene(game.loadingScene)) SceneManager::setLoadingScene(game.loadingScene);
    if (game.loadingDelay != 0.0f) SceneManager::setLoadingDelay(game.loadingDelay);
    if (game.loadingTimeout != 5.0f) SceneManager::setLoadingTimeout(game.loadingTimeout);

    const std::string& requested = RunControl::options().scene;
    const GameScene* start = requested.empty() ? game.findScene(game.startScene) : game.findScene(requested);
    if (!start && !game.scenes.empty()) start = &game.scenes.front();
    if (start) SceneManager::loadScene(start->id);
}
