// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneManager.h"
#include "Engine.h"
#include "Scene.h"
#include "Log.h"
#include "memory/HeapTrim.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/MeshSystem.h"
#include "pixelart/BitmapFontPool.h"
#include "pool/TexturePool.h"
#include "pool/TextureDataPool.h"
#include "pool/SoundPool.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "animation/CompiledController.h"
#include "animation/ControllerDef.h"
#include "animation/AnimationClip.h"
#include "animation/ModelSubClips.h"
#include "animation/VertexAnimation.h"
#include "pool/VertexAnimationPool.h"
#include "thread/ResourceProgress.h"
#include "script/ScriptRuntime.h"
#include "manager/ObjectManager.h"
#include "subsystem/UIDocumentSystem.h"
#include "component/Transform.h"
#include "component/CameraComponent.h"
#include "component/ScriptComponent.h"
#include "ecs/EntityManager.h"
#include "audio/AudioMarkers.h"

#include <algorithm>

using namespace jasperengine;

std::vector<SceneManager::SceneEntry> SceneManager::entries;
uint32_t SceneManager::currentId = 0;
std::optional<uint32_t> SceneManager::pendingId;
std::map<uint32_t, Scene*> SceneManager::scenePtrs;
bool SceneManager::factoryRunning = false;
std::vector<Scene*> SceneManager::unloadingScenes;

uint32_t SceneManager::loadingSceneId = 0;
float SceneManager::loadingDelay = 0.0f;
float SceneManager::loadingTimeout = 5.0f;
SceneManager::LoadingState SceneManager::loadingState = SceneManager::LoadingState::None;
bool SceneManager::loadingSceneShown = false;
bool SceneManager::loadingSceneReady = false;
double SceneManager::loadingStateTime = 0.0;
size_t SceneManager::loadingCount = 0;
double SceneManager::loadingProgressTime = 0.0;
float SceneManager::loadingProgress = 0.0f;
int SceneManager::loadingHolds = 0;

std::vector<SceneManager::Preload> SceneManager::preloads;

Scene* SceneManager::persistentScene = nullptr;
std::function<Scene*()> SceneManager::persistentCreate;
std::function<void(Scene*)> SceneManager::persistentDestroy;
std::vector<SceneManager::PendingMove> SceneManager::pendingMoves;
bool SceneManager::flushingMoves = false;
int SceneManager::startingScenes = 0;

namespace {

// destroyPersistentScene is releasing its scripts: nothing makes a new one meanwhile
bool destroyingPersistent = false;

bool isRootObject(Scene* scene, Entity entity) {
    const Transform* transform = scene->findComponent<Transform>(entity);
    return !transform || transform->parent == NULL_ENTITY;
}

// `entity` is `root` or one of its descendants
bool isInHierarchyOf(Scene* scene, Entity entity, Entity root) {
    for (int depth = 0; entity != NULL_ENTITY && depth < 4096; depth++) {
        if (entity == root) {
            return true;
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        entity = transform ? transform->parent : NULL_ENTITY;
    }
    return false;
}

}

std::vector<uint32_t> SceneManager::buildSceneStackIds(uint32_t id, const std::vector<uint32_t>& sceneIds) {
    std::vector<uint32_t> result;
    result.reserve(sceneIds.size() + 1);

    auto addId = [&result](uint32_t sceneId) {
        if (std::find(result.begin(), result.end(), sceneId) == result.end()) {
            result.push_back(sceneId);
        }
    };

    addId(id);
    for (uint32_t sceneId : sceneIds) {
        addId(sceneId);
    }

    return result;
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory) {
    registerScene(id, name, std::move(loadFactory), nullptr, std::vector<uint32_t>{id});
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, const std::vector<uint32_t>& sceneIds) {
    registerScene(id, name, std::move(loadFactory), nullptr, sceneIds);
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::initializer_list<uint32_t> sceneIds) {
    registerScene(id, name, std::move(loadFactory), nullptr, std::vector<uint32_t>(sceneIds));
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory) {
    registerScene(id, name, std::move(loadFactory), std::move(addFactory), std::vector<uint32_t>{id});
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory, const std::vector<uint32_t>& sceneIds, std::function<SceneResources()> resources) {
    std::vector<uint32_t> stackSceneIds = buildSceneStackIds(id, sceneIds);

    // Overwrite if the id already exists
    for (auto& entry : entries) {
        if (entry.id == id) {
            entry.name = name;
            entry.loadFactory = std::move(loadFactory);
            entry.addFactory = std::move(addFactory);
            entry.sceneIds = std::move(stackSceneIds);
            entry.resources = std::move(resources);
            return;
        }
    }
    entries.push_back({id, name, std::move(loadFactory), std::move(addFactory), std::move(stackSceneIds), std::move(resources)});
}

bool SceneManager::loadScene(const std::string& name) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (entries[i].name == name) {
            return loadScene(entries[i].id);
        }
    }
    Log::error("SceneManager: scene '%s' not found", name.c_str());
    return false;
}

SceneManager::SceneEntry* SceneManager::findEntry(uint32_t id) {
    for (auto& entry : entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void SceneManager::runFactory(uint32_t id) {
    if (!findEntry(id)) return;

    // what was marked to persist leaves the scenes about to go, with its state
    flushPersistentMoves();

    SceneEntry* entry = findEntry(id);
    if (!entry) return;

#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::sceneLoad(entry->name, "load");
#endif

    // Copied for the same reason as in addChildScene: the factory runs scripts
    std::function<void()> loadFactory = entry->loadFactory;

    std::vector<Scene*> loadingScenes;
    if (loadingSceneShown) {
        loadingScenes = getRunningScenes(loadingSceneId);
    }

    // every scene whose scripts run now: the factory tears them down (a stack loaded again
    // too), except the loading scene's, still on screen, and the persistent scene
    const std::vector<Scene*> previous = ScriptRuntime::getStartedScenes();

    Engine::removeAllScenes();

    // still running, so the factory does not delete them
    for (Scene* scene : loadingScenes) {
        Engine::addSceneLayer(scene);
    }
    if (persistentScene) {
        Engine::addSceneLayer(persistentScene);
    }

    std::vector<Scene*> unloading;
    for (Scene* scene : previous) {
        if (scene != persistentScene && std::find(loadingScenes.begin(), loadingScenes.end(), scene) == loadingScenes.end()) {
            unloading.push_back(scene);
        }
    }

    currentId = id;
    // Time.timeSinceLevelLoad counts from this load
    Engine::markLevelLoad();
    const bool nested = factoryRunning;
    factoryRunning = true;
    std::vector<Scene*> nestedUnloading = std::move(unloadingScenes);
    unloadingScenes = unloading;
    // Unity: the old scenes' onDisable and onDestroy, before the new scenes' init (a singleton
    // clearing itself in onDestroy does not clobber the new one)
    ScriptRuntime::unloadScenes(unloading);
    loadFactory();
    unloadingScenes = std::move(nestedUnloading);
    factoryRunning = nested;

    // above the new stack, below the loading scene
    raisePersistentScene();
    if (loadingSceneShown) {
        raiseLoadingScene();
    }
}

bool SceneManager::isFactoryRunning() {
    return factoryRunning;
}

const std::vector<Scene*>& SceneManager::getUnloadingScenes() {
    return unloadingScenes;
}

bool SceneManager::loadScene(uint32_t id) {
    if (!findEntry(id)) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    // input callbacks run between frames, but still inside the scenes being replaced; a
    // script's init asking from inside a load factory (the first scene's, before any frame and
    // before it is the main scene) would have the scene it runs in deleted under it. The start
    // scene waits for the first frame too when a loading scene can cover it (applyPendingLoad)
    const bool coverStart = loadingSceneId != 0 && canShowLoadingScene(id);
    if (Engine::isFrameRunning() || Engine::getMainScene() || factoryRunning || coverStart) {
        if (pendingId && *pendingId != id) {
            Log::warn("SceneManager: scene %u replaces pending scene %u", id, *pendingId);
        }
        pendingId = id;
        return true;
    }

    startLoading(id);
    return true;
}

bool SceneManager::isLoadPending() {
    return pendingId.has_value();
}

void SceneManager::applyPendingLoad() {
    if (!pendingId) return;

    uint32_t id = *pendingId;

    if (loadingState != LoadingState::Covering && canShowLoadingScene(id) && addChildScene(loadingSceneId)) {
        raiseLoadingScene();
        loadingSceneShown = true;
        loadingSceneReady = false;
        loadingState = LoadingState::Covering;
        loadingStateTime = Engine::getSystemTime();
        resetLoadProgress();
        return;
    }

    if (loadingState == LoadingState::Covering) {
        // the switch freezes on the last presented frame, which must show the loading scene
        if (!loadingSceneReady || Engine::getSystemTime() - loadingStateTime < loadingDelay) {
            return;
        }
    }

    startLoading(id);
}

std::vector<Scene*> SceneManager::getRunningScenes(uint32_t id) {
    std::vector<Scene*> scenes;
    SceneEntry* entry = findEntry(id);
    if (!entry) return scenes;

    for (uint32_t sceneId : entry->sceneIds) {
        Scene* scene = getScenePtr(sceneId);
        if (scene && Engine::isSceneRunning(scene)) {
            scenes.push_back(scene);
        }
    }

    return scenes;
}

bool SceneManager::canShowLoadingScene(uint32_t id) {
    SceneEntry* loadingEntry = findEntry(loadingSceneId);
    SceneEntry* entry = findEntry(id);
    if (!loadingEntry || !entry) return false;

    // not for a stack that includes it
    for (uint32_t sceneId : loadingEntry->sceneIds) {
        if (std::find(entry->sceneIds.begin(), entry->sceneIds.end(), sceneId) != entry->sceneIds.end()) {
            return false;
        }
    }

    return true;
}

void SceneManager::raiseLoadingScene() {
    for (Scene* scene : getRunningScenes(loadingSceneId)) {
        if (scene == Engine::getMainScene()) continue;
        Engine::removeScene(scene);
        Engine::addSceneLayer(scene);
    }
}

void SceneManager::startLoading(uint32_t id) {
    pendingId.reset();
    // before the factory, so the scripts it builds see isLoading()
    loadingState = LoadingState::Loading;
    runFactory(id);
    resetLoadProgress();
}

void SceneManager::resetLoadProgress() {
    loadingCount = 0;
    loadingProgressTime = Engine::getSystemTime();
    loadingProgress = 0.0f;
}

bool SceneManager::isLoadProgressing(size_t loaded, double now) {
    if (loaded != loadingCount || ResourceProgress::hasActiveBuilds()) {
        loadingCount = loaded;
        loadingProgressTime = now;
    }

    return now - loadingProgressTime < loadingTimeout;
}

void SceneManager::getLoadCount(uint32_t id, size_t& loaded, size_t& total) {
    loaded = 0;
    total = 0;

    for (Scene* scene : getRunningScenes(id)) {
        size_t sceneLoaded = 0;
        size_t sceneTotal = 0;
        scene->getSystem<RenderSystem>()->getLoadCount(sceneLoaded, sceneTotal);
        loaded += sceneLoaded;
        total += sceneTotal;

        scene->getSystem<AudioSystem>()->getLoadCount(sceneLoaded, sceneTotal);
        loaded += sceneLoaded;
        total += sceneTotal;
    }
}

bool SceneManager::setLoadingScene(const std::string& name) {
    if (name.empty()) {
        loadingSceneId = 0;
        return true;
    }

    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return setLoadingScene(id);
}

bool SceneManager::setLoadingScene(uint32_t id) {
    if (id != 0 && !findEntry(id)) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    loadingSceneId = id;
    return true;
}

uint32_t SceneManager::getLoadingSceneId() {
    return loadingSceneId;
}

void SceneManager::setLoadingDelay(float seconds) {
    loadingDelay = std::max(0.0f, seconds);
}

float SceneManager::getLoadingDelay() {
    return loadingDelay;
}

void SceneManager::setLoadingTimeout(float seconds) {
    loadingTimeout = std::max(0.0f, seconds);
}

float SceneManager::getLoadingTimeout() {
    return loadingTimeout;
}

bool SceneManager::isLoading() {
    return pendingId.has_value() || loadingState != LoadingState::None;
}

float SceneManager::getLoadingProgress() {
    if (pendingId || loadingState == LoadingState::Covering) return 0.0f;
    if (loadingState == LoadingState::Loading) return loadingProgress;

    return 1.0f;
}

bool SceneManager::holdLoading() {
    if (!isLoading()) return false;

    loadingHolds++;
    return true;
}

void SceneManager::releaseLoading() {
    if (loadingHolds > 0) loadingHolds--;
}

bool SceneManager::isCoveredByLoading(Scene* scene) {
    if (!loadingSceneShown) return false;

    std::vector<Scene*> loadingScenes = getRunningScenes(loadingSceneId);
    return std::find(loadingScenes.begin(), loadingScenes.end(), scene) == loadingScenes.end();
}

void SceneManager::setCurrentScene(uint32_t id) {
    pendingId.reset();
    currentId = id;
    Engine::markLevelLoad();
    loadingState = LoadingState::Loading;
    resetLoadProgress();
}

SceneManager::Preload* SceneManager::findPreload(uint32_t id) {
    for (auto& preload : preloads) {
        if (preload.id == id) return &preload;
    }
    return nullptr;
}

bool SceneManager::preloadScene(const std::string& name) {
    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return preloadScene(id);
}

bool SceneManager::preloadScene(uint32_t id) {
    if (!findEntry(id)) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    // started at the end of the frame, not inside the calling script
    Preload* preload = findPreload(id);
    if (!preload) {
        preloads.push_back({id});
    } else if (preload->cancelled) {
        preload->cancelled = false;
        preload->started = false;
    }
    return true;
}

float SceneManager::getPreloadProgress(const std::string& name) {
    return getPreloadProgress(getSceneId(name));
}

float SceneManager::getPreloadProgress(uint32_t id) {
    Preload* preload = findPreload(id);
    if (!preload || !preload->started || preload->cancelled) return 0.0f;
    if (preload->total == 0) return 1.0f;

    const SceneResources& pending = preload->pending;
    size_t remaining = pending.textures.size() + pending.sounds.size() + pending.models.size() + pending.clips.size() + pending.vats.size() +
                       pending.masks.size() + pending.controllers.size() + pending.bitmapFonts.size();
    return (float)(preload->total - remaining) / (float)preload->total;
}

void SceneManager::cancelPreload(const std::string& name) {
    cancelPreload(getSceneId(name));
}

void SceneManager::cancelPreload(uint32_t id) {
    Preload* preload = findPreload(id);
    if (!preload) return;

    // loads in flight still end, then everything no scene uses is released
    preload->cancelled = true;
    preload->held.clear();
    if (!Engine::isAsyncLoading()) {
        preload->pending.textures.clear();
        preload->pending.sounds.clear();
        preload->pending.clips.clear();
        preload->pending.vats.clear();
        preload->pending.masks.clear();
        preload->pending.controllers.clear();
        preload->pending.bitmapFonts.clear();
    }
}

void SceneManager::startPreload(uint32_t id) {
    SceneEntry* entry = findEntry(id);
    Preload* preload = findPreload(id);
    if (!entry || !preload) return;

    // copied, a resource function can register scenes and add preloads
    std::vector<uint32_t> sceneIds = entry->sceneIds;
    SceneResources pending = preload->pending;

    auto addFile = [](std::vector<std::string>& files, const std::string& file) {
        if (std::find(files.begin(), files.end(), file) == files.end()) {
            files.push_back(file);
        }
    };

    for (uint32_t sceneId : sceneIds) {
        // a running scene has its files already
        Scene* scene = getScenePtr(sceneId);
        if (scene && Engine::isSceneRunning(scene)) continue;

        SceneEntry* sceneEntry = findEntry(sceneId);
        if (!sceneEntry || !sceneEntry->resources) continue;

        SceneResources resources = sceneEntry->resources();
        for (const Texture& texture : resources.textures) {
            auto sameRender = [&texture](const Texture& other) { return other.getRenderId() == texture.getRenderId(); };
            if (std::none_of(pending.textures.begin(), pending.textures.end(), sameRender)) {
                pending.textures.push_back(texture);
            }
        }
        for (const std::string& sound : resources.sounds) {
            addFile(pending.sounds, sound);
        }
        for (const std::string& model : resources.models) {
            addFile(pending.models, model);
        }
        for (const std::string& clip : resources.clips) {
            addFile(pending.clips, clip);
        }
        for (const std::string& vat : resources.vats) {
            addFile(pending.vats, vat);
        }
        for (const std::string& mask : resources.masks) {
            addFile(pending.masks, mask);
        }
        for (const std::string& controller : resources.controllers) {
            addFile(pending.controllers, controller);
        }
        for (const std::string& font : resources.bitmapFonts) {
            addFile(pending.bitmapFonts, font);
        }
    }

    preload = findPreload(id);
    if (!preload) return;

    preload->started = true;
    preload->total = pending.textures.size() + pending.sounds.size() + pending.models.size() + pending.clips.size() + pending.vats.size() +
                       pending.masks.size() + pending.controllers.size() + pending.bitmapFonts.size();
    preload->pending = std::move(pending);
}

void SceneManager::releasePreload(Preload& preload) {
    preload.held.clear();
    for (const auto& release : preload.releases) {
        release();
    }
    preload.releases.clear();
}

void SceneManager::updatePreloads() {
    // a synchronous load stalls this thread, so only one goes per frame
    const bool oneFile = !Engine::isAsyncLoading();

    // by index, a resource function can add a preload
    for (size_t i = 0; i < preloads.size();) {
        if (!preloads[i].started && !preloads[i].cancelled) {
            startPreload(preloads[i].id);
        }

        Preload& preload = preloads[i];
        SceneResources& pending = preload.pending;

        auto keep = [&preload](std::shared_ptr<void> data, std::function<void()> release) {
            if (!data) return;
            if (!preload.cancelled) preload.held.push_back(std::move(data));
            preload.releases.push_back(std::move(release));
        };

        // true once a load ended, keeping what it loaded
        auto finished = [&keep](const auto& result, std::function<void()> release) {
            if (result.state == ResourceLoadState::Loading) return false;
            keep(result.data, std::move(release));
            return true;
        };

        for (auto it = pending.textures.begin(); it != pending.textures.end();) {
            const std::string id = it->getId();
            const std::string renderId = it->getRenderId();
            auto release = [id, renderId]() {
                TexturePool::remove(renderId);
                TextureDataPool::remove(id);
            };

            // on the GPU for a running scene, held so the switch does not free it
            if (std::shared_ptr<TextureRender> render = TexturePool::get(renderId)) {
                keep(render, release);
                it = pending.textures.erase(it);
                continue;
            }

            it = finished(it->load(), release) ? pending.textures.erase(it) : it + 1;
            if (oneFile) return;
        }

        for (auto it = pending.sounds.begin(); it != pending.sounds.end();) {
            // "<file>" or "<file>#<load type>" (a source's Load Type other than Auto)
            std::string file;
            AudioLoadType loadType = AudioLoadType::Auto;
            SoundPool::parseClipRef(*it, file, loadType);
            const SoundLoadResult result = SoundPool::loadClip(file, loadType);
            const std::string id = result.id;
            auto release = [id]() { SoundPool::remove(id); };
            it = finished(result, release) ? pending.sounds.erase(it) : it + 1;
            if (oneFile) return;
        }

        for (auto it = pending.models.begin(); it != pending.models.end();) {
            std::shared_ptr<void> data;
            if (!MeshSystem::preloadModel(*it, data)) {
                ++it;
                continue;
            }

            const std::string file = *it;
            keep(data, [file]() { MeshSystem::releasePreloadedModel(file); });
            it = pending.models.erase(it);
        }

        // a sub-clip waits for its model's preload; a sub-clip or a large .anim builds on the
        // thread pool in async loading (AnimationClipPool::pollOrLoad)
        for (auto it = pending.clips.begin(); it != pending.clips.end();) {
            anim::SubClipReference reference;
            if (anim::parseSubClipReference(*it, reference)) {
                // the same file however its path is spelled
                const std::string modelKey = AnimationClipPool::getKey(reference.model);
                auto sameModel = [&modelKey](const std::string& model) { return AnimationClipPool::getKey(model) == modelKey; };
                if (std::any_of(pending.models.begin(), pending.models.end(), sameModel)) {
                    ++it;
                    continue;
                }
            }
            const std::string clip = *it;
            std::string error;
            bool loading = false;
            std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::pollOrLoad(clip, loading, &error);
            if (loading) {
                ++it;
                continue;
            }
            if (!loaded) {
                Log::warn("Preload of animation clip %s failed: %s", clip.c_str(), error.c_str());
            }
            keep(std::const_pointer_cast<AnimationClip>(loaded), [clip]() { AnimationClipPool::releaseUnused(clip); });
            it = pending.clips.erase(it);
            if (oneFile) return;
        }

        // Vertex Animations: the JSON and the payload of the .vat.glb read on this thread (the
        // GPU texture is made when it first draws)
        for (auto it = pending.vats.begin(); it != pending.vats.end();) {
            const std::string vat = *it;
            std::string error;
            std::shared_ptr<const vat::VertexAnimation> loaded = VertexAnimationPool::getOrLoad(vat, &error);
            if (!loaded) {
                Log::warn("Preload of vertex animation %s failed: %s", vat.c_str(), error.c_str());
            }
            keep(std::const_pointer_cast<vat::VertexAnimation>(loaded), []() { VertexAnimationPool::clearUnused(); });
            it = pending.vats.erase(it);
            if (oneFile) return;
        }

        // Avatar Masks and Animator Controllers: small JSON files, read on this thread
        for (auto it = pending.masks.begin(); it != pending.masks.end();) {
            const std::string mask = *it;
            std::string error;
            std::shared_ptr<const anim::AvatarMaskDef> loaded = AnimatorAssetPool::mask(mask, &error);
            if (!loaded) {
                Log::warn("Preload of avatar mask %s failed: %s", mask.c_str(), error.c_str());
            }
            keep(std::const_pointer_cast<anim::AvatarMaskDef>(loaded), []() { AnimatorAssetPool::clearUnused(); });
            it = pending.masks.erase(it);
            if (oneFile) return;
        }
        for (auto it = pending.controllers.begin(); it != pending.controllers.end();) {
            const std::string controller = *it;
            std::string error;
            std::shared_ptr<const anim::CompiledController> loaded = AnimatorAssetPool::controller(controller, &error);
            if (!loaded) {
                Log::warn("Preload of animator controller %s failed: %s", controller.c_str(), error.c_str());
            }
            keep(std::const_pointer_cast<anim::CompiledController>(loaded), []() { AnimatorAssetPool::clearUnused(); });
            it = pending.controllers.erase(it);
            if (oneFile) return;
        }

        // bitmap fonts: a cooked .jfont reads in well under a millisecond, on this thread
        for (auto it = pending.bitmapFonts.begin(); it != pending.bitmapFonts.end();) {
            const std::string font = *it;
            pixelart::BitmapFontReport report;
            std::shared_ptr<pixelart::BitmapFont> loaded = pixelart::BitmapFontPool::get(font, &report);
            if (!loaded) {
                Log::warn("Preload of bitmap font %s failed: %s", font.c_str(),
                          report.errors.empty() ? "unreadable" : report.errors[0].message.c_str());
            }
            keep(loaded, []() { pixelart::BitmapFontPool::clearUnused(); });
            it = pending.bitmapFonts.erase(it);
            if (oneFile) return;
        }

        const bool inFlight = !pending.textures.empty() || !pending.sounds.empty() || !pending.models.empty() || !pending.clips.empty() ||
                              !pending.vats.empty() || !pending.masks.empty() || !pending.controllers.empty() || !pending.bitmapFonts.empty();
        if (preload.cancelled && !inFlight) {
            releasePreload(preload);
            preloads.erase(preloads.begin() + i);
        } else {
            i++;
        }
    }
}

void SceneManager::updateLoading() {
    updatePreloads();

    if (loadingState == LoadingState::None) return;

    double now = Engine::getSystemTime();

    if (loadingState == LoadingState::Covering) {
        if (!loadingSceneReady) {
            size_t loaded;
            size_t total;
            getLoadCount(loadingSceneId, loaded, total);
            loadingSceneReady = loaded >= total || !isLoadProgressing(loaded, now);
        }
        return;
    }

    if (loadingState == LoadingState::Loading) {
        size_t loaded;
        size_t total;
        getLoadCount(currentId, loaded, total);

        // never moves back when the stack creates more to load
        if (total > 0) {
            loadingProgress = std::max(loadingProgress, (float)loaded / (float)total);
        }

        if (loaded < total) {
            if (isLoadProgressing(loaded, now)) return;
            Log::warn("SceneManager: %zu of %zu resources of '%s' did not load",
                total - loaded, total, getSceneName(currentId).c_str());
        }

        loadingState = LoadingState::Loaded;
        // a handler can hold the load or start another one
        Engine::onSceneLoaded.call();
    }

    if (loadingHolds > 0 || pendingId) return;

    if (loadingSceneShown) {
        removeChildScene(loadingSceneId);
        loadingSceneShown = false;
    }
    loadingState = LoadingState::None;

    // the new scenes hold their own files now
    cancelPreload(currentId);
    // what the replaced scenes and the decoders freed goes back to the system
    memory::requestHeapTrim();
}

bool SceneManager::addChildScene(uint32_t id) {
    SceneEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::sceneLoad(entry->name, "add");
#endif

    // The factory runs scripts, which can register scenes and reallocate entries
    std::vector<uint32_t> sceneIds = entry->sceneIds;
    std::function<void()> addFactory = entry->addFactory;

    // Even when the scenes exist: a registered pointer does not mean the stack is ready to
    // show. The editor keeps the pointers of a stack it switched away from after tearing its
    // scripts down, and only the factory puts them back. Both factories are idempotent.
    if (addFactory) {
        addFactory();
    }

    for (uint32_t sceneId : sceneIds) {
        if (!getScenePtr(sceneId)) {
            Log::error("SceneManager: scene id %u is not loaded", sceneId);
            return false;
        }
    }

    // In stack order, so the root stays below the layers it owns
    for (uint32_t sceneId : sceneIds) {
        Engine::addSceneLayer(getScenePtr(sceneId));
    }

    // the persistent scene stays above the game's scenes, the loading scene above it
    raisePersistentScene();
    if (loadingSceneShown) {
        raiseLoadingScene();
    }

    return true;
}

bool SceneManager::addChildScene(const std::string& name) {
    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return addChildScene(id);
}

bool SceneManager::removeChildScene(uint32_t id) {
    SceneEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    bool removedScene = false;
    for (auto it = entry->sceneIds.rbegin(); it != entry->sceneIds.rend(); ++it) {
        Scene* scene = getScenePtr(*it);
        if (!scene || scene == Engine::getMainScene() || !Engine::isSceneRunning(scene)) {
            continue;
        }

        Engine::removeScene(scene);
        removedScene = true;
    }

    return removedScene;
}

bool SceneManager::removeChildScene(const std::string& name) {
    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return removeChildScene(id);
}

uint32_t SceneManager::getSceneId(const std::string& name) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (entries[i].name == name) return entries[i].id;
    }
    return 0;
}

std::string SceneManager::getSceneName(uint32_t id) {
    const SceneEntry* entry = findEntry(id);
    return entry ? entry->name : "";
}

std::string SceneManager::getSceneNameOf(const Scene* scene) {
    if (!scene) return "";
    for (const auto& [id, ptr] : scenePtrs) {
        if (ptr == scene) return getSceneName(id);
    }
    return "";
}

std::vector<std::string> SceneManager::getSceneNames() {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) {
        names.push_back(entry.name);
    }
    return names;
}

int SceneManager::getSceneCount() {
    return (int)entries.size();
}

uint32_t SceneManager::getCurrentSceneId() {
    return currentId;
}

std::string SceneManager::getCurrentSceneName() {
    return getSceneName(currentId);
}

void SceneManager::clearAll() {
    destroyPersistentScene();

    entries.clear();
    currentId = 0;
    pendingId.reset();
    scenePtrs.clear();

    loadingSceneId = 0;
    loadingDelay = 0.0f;
    loadingTimeout = 5.0f;
    loadingState = LoadingState::None;
    loadingSceneShown = false;
    loadingHolds = 0;

    // a model still parsing would keep its build open
    for (Preload& preload : preloads) {
        for (const std::string& model : preload.pending.models) {
            MeshSystem::cancelPreloadModel(model);
        }
        releasePreload(preload);
    }
    preloads.clear();
}

void SceneManager::setScenePtr(uint32_t id, Scene* scene) {
    if (scene) {
        scenePtrs[id] = scene;
    } else {
        scenePtrs.erase(id);
    }
}

Scene* SceneManager::getScenePtr(uint32_t id) {
    auto it = scenePtrs.find(id);
    return (it != scenePtrs.end()) ? it->second : nullptr;
}

void SceneManager::removeScenePtr(uint32_t id) {
    scenePtrs.erase(id);
}

std::vector<Scene*> SceneManager::getScenePtrs() {
    std::vector<Scene*> scenes;
    scenes.reserve(scenePtrs.size());
    for (const auto& pair : scenePtrs) {
        scenes.push_back(pair.second);
    }
    return scenes;
}

// ---- persistent objects (DontDestroyOnLoad)

void SceneManager::setPersistentSceneFactory(std::function<Scene*()> create, std::function<void(Scene*)> destroy) {
    persistentCreate = std::move(create);
    persistentDestroy = std::move(destroy);
}

Scene* SceneManager::getPersistentScene() {
    return persistentScene;
}

Scene* SceneManager::ensurePersistentScene() {
    if (persistentScene) {
        return persistentScene;
    }
    if (destroyingPersistent) {
        return nullptr;
    }

    Scene* scene = persistentCreate ? persistentCreate() : new Scene();
    if (!scene) {
        return nullptr;
    }
    persistentScene = scene;
    setScenePtr(PERSISTENT_SCENE_ID, scene);

    // started from the beginning: what moves in keeps running, what is instantiated in it
    // starts, its HTML documents run their scripts
    ScriptRuntime::awakeScene(scene);
    scene->getSystem<UIDocumentSystem>()->startDocumentScripts();

    Engine::addSceneLayer(scene);
    // without a view nothing loads it yet: its default camera (the UI camera) is made now
    if (scene->getCamera() == NULL_ENTITY) {
        scene->load();
    }
    raisePersistentScene();
    if (loadingSceneShown) {
        raiseLoadingScene();
    }
    return scene;
}

void SceneManager::raisePersistentScene() {
    Scene* scene = persistentScene;
    if (!scene || scene == Engine::getMainScene()) {
        return;
    }

    // already on top of every scene but the shown loading scene's
    const std::vector<Scene*> layers = Engine::getSceneLayers();
    auto it = std::find(layers.begin(), layers.end(), scene);
    if (it != layers.end()) {
        std::vector<Scene*> loadingScenes;
        if (loadingSceneShown) {
            loadingScenes = getRunningScenes(loadingSceneId);
        }
        const bool onTop = std::all_of(it + 1, layers.end(), [&loadingScenes](Scene* above) {
            return std::find(loadingScenes.begin(), loadingScenes.end(), above) != loadingScenes.end();
        });
        if (onTop) {
            return;
        }
    }

    Engine::removeScene(scene);
    Engine::addSceneLayer(scene);
}

void SceneManager::destroyPersistentScene() {
    Scene* scene = persistentScene;
    // moves to or from it end here
    pendingMoves.erase(std::remove_if(pendingMoves.begin(), pendingMoves.end(), [scene](const PendingMove& move) {
        return scene && (move.to == scene || move.from == scene);
    }), pendingMoves.end());
    if (!scene) {
        return;
    }

    // its scripts go (the quit pass ran their onDestroy; otherwise it runs now), while it is
    // still the persistent scene, so a callback cannot make another one
    destroyingPersistent = true;
    std::vector<Entity> scripted;
    auto scripts = scene->getComponentArray<ScriptComponent>();
    for (size_t i = 0; i < scripts->size(); i++) {
        scripted.push_back(scripts->getEntity(i));
    }
    ObjectManager::cleanupScripts(scene, scripted);
    scene->getSystem<UIDocumentSystem>()->releaseScriptState();
    destroyingPersistent = false;

    Engine::removeScene(scene);
    if (getScenePtr(PERSISTENT_SCENE_ID) == scene) {
        removeScenePtr(PERSISTENT_SCENE_ID);
    }
    persistentScene = nullptr;

    if (persistentDestroy) {
        persistentDestroy(scene);
    } else {
        // ~Scene stops its sounds
        delete scene;
    }
}

bool SceneManager::queueMove(Scene* from, Entity root, Scene* to, const char* caller) {
    if (!from || root == NULL_ENTITY || !from->isEntityCreated(root)) {
        Log::warn("%s: the GameObject does not exist", caller);
        return false;
    }
    if (!isRootObject(from, root)) {
        Log::warn("%s only works for root GameObjects ('%s' has a parent)", caller, from->getEntityName(root).c_str());
        return false;
    }

    // a move of it queued before is replaced
    pendingMoves.erase(std::remove_if(pendingMoves.begin(), pendingMoves.end(), [from, root](const PendingMove& move) {
        return move.from == from && move.root == root;
    }), pendingMoves.end());
    if (to != from) {
        pendingMoves.push_back({from, root, to});
    }
    return true;
}

bool SceneManager::dontDestroyOnLoad(Scene* scene, Entity root) {
    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)) {
        Log::warn("DontDestroyOnLoad: the GameObject does not exist");
        return false;
    }
    if (!isRootObject(scene, root)) {
        Log::warn("DontDestroyOnLoad only works for root GameObjects ('%s' has a parent)", scene->getEntityName(root).c_str());
        return false;
    }
    if (persistentScene && scene == persistentScene) {
        // a move out of it asked for this frame is called off: it stays
        pendingMoves.erase(std::remove_if(pendingMoves.begin(), pendingMoves.end(), [scene, root](const PendingMove& move) {
            return move.from == scene && move.root == root;
        }), pendingMoves.end());
        return true;
    }
    if (isPersistent(scene, root)) {
        return true;
    }

    Scene* target = ensurePersistentScene();
    if (!target) {
        Log::warn("DontDestroyOnLoad: '%s' stays, there is no persistent scene while the game is not running",
                  scene->getEntityName(root).c_str());
        return false;
    }
    return queueMove(scene, root, target, "DontDestroyOnLoad");
}

bool SceneManager::isPersistent(Scene* scene, Entity entity) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return false;
    }
    if (persistentScene && scene == persistentScene) {
        // ...unless its root is about to move out (moveGameObjectToScene)
        for (const PendingMove& move : pendingMoves) {
            if (move.from == scene && isInHierarchyOf(scene, entity, move.root)) {
                return false;
            }
        }
        return true;
    }
    for (const PendingMove& move : pendingMoves) {
        if (move.to == persistentScene && move.from == scene && isInHierarchyOf(scene, entity, move.root)) {
            return true;
        }
    }
    return false;
}

bool SceneManager::moveGameObjectToScene(Scene* from, Entity root, Scene* to) {
    if (!to || (to != persistentScene && !Engine::isSceneRunning(to))) {
        Log::error("SceneManager.moveGameObjectToScene: the target scene is not loaded");
        return false;
    }
    return queueMove(from, root, to, "SceneManager.moveGameObjectToScene");
}

void SceneManager::flushPersistentMoves() {
    if (flushingMoves || pendingMoves.empty()) {
        return;
    }
    flushingMoves = true;
    std::vector<PendingMove> moves;
    moves.swap(pendingMoves);
    for (const PendingMove& move : moves) {
        // destroyed meanwhile: the destroy wins
        if (!move.from || !move.to || move.from == move.to || !move.from->isEntityCreated(move.root)) {
            continue;
        }
        const std::vector<Entity> moved = ObjectManager::moveToScene(move.from, move.root, move.to);
        if (moved.empty() || move.to != persistentScene) {
            continue;
        }
        // the first Camera a persistent GameObject brings is the persistent scene's camera
        // (until then it draws with its default UI camera, a system entity)
        const Entity camera = persistentScene->getCamera();
        if (camera == NULL_ENTITY || camera <= EntityManager::lastSystemEntity() ||
                !persistentScene->findComponent<CameraComponent>(camera)) {
            for (Entity entity : moved) {
                if (persistentScene->findComponent<CameraComponent>(entity)) {
                    persistentScene->setCamera(entity);
                    break;
                }
            }
        }
    }
    flushingMoves = false;
}

bool SceneManager::hasPendingMoves(Scene* scene) {
    if (!scene) {
        return !pendingMoves.empty();
    }
    return std::any_of(pendingMoves.begin(), pendingMoves.end(), [scene](const PendingMove& move) {
        return move.from == scene || move.to == scene;
    });
}

std::vector<std::pair<Scene*, Entity>> SceneManager::getPendingPersistentRoots() {
    std::vector<std::pair<Scene*, Entity>> roots;
    for (const PendingMove& move : pendingMoves) {
        if (persistentScene && move.to == persistentScene) {
            roots.push_back({move.from, move.root});
        }
    }
    return roots;
}

void SceneManager::forgetPendingMoves(Scene* scene) {
    if (!scene) {
        return;
    }
    pendingMoves.erase(std::remove_if(pendingMoves.begin(), pendingMoves.end(), [scene](const PendingMove& move) {
        return move.from == scene || move.to == scene;
    }), pendingMoves.end());
    // deleted by someone else (its factory's owner): never reached again
    if (scene == persistentScene) {
        persistentScene = nullptr;
        if (getScenePtr(PERSISTENT_SCENE_ID) == scene) {
            removeScenePtr(PERSISTENT_SCENE_ID);
        }
    }
}

void SceneManager::beginStartingScenes() {
    startingScenes++;
}

void SceneManager::endStartingScenes() {
    if (startingScenes > 0) {
        startingScenes--;
    }
}

bool SceneManager::isStartingScenes() {
    return factoryRunning || startingScenes > 0;
}
