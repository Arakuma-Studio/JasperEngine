// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "manager/ObjectManager.h"
#include "component/OnScreenControlComponents.h"
#include "Engine.h"
#include "component/ComponentEvents.h"
#include "subsystem/ParticleSubsystem.h"
#include "shader/ShaderGlobals.h"
#include "Log.h"
#include "log/Logger.h"
#include "diag/CrashHandler.h"
#include "diag/HangWatchdog.h"
#include "script/LuaWatchdog.h"
#include "util/Scheduler.h"
#include "Scene.h"
#include "Input.h"
#include "Screen.h"
#include "io/UserSettings.h"
#include "input/InputActions.h"
#include "debug/DebugOverlay.h"
#include "render/SystemRender.h"
#include "shader/ShaderWarmup.h"
#include "script/LuaBinding.h"
#include "script/ScriptScheduler.h"
#include "script/ScriptRuntime.h"
#include "lua.hpp"
#include "manager/SceneManager.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "audio/AudioCapture.h"
#include "audio/AudioMarkers.h"
#include "subsystem/CameraControlSystem.h"
#include "audio/AudioMixer.h"
#if JASPERENGINE_SOUND_LAYER
#include "audio/MusicPlayer.h"
#endif
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIClock.h"
#if JASPERENGINE_HTMLUI
#include "htmlui/anim/AnimEngine.h"
#endif
#include "util/StringUtils.h"
#include "util/Utf16Combiner.h"
#include "util/PlatformRules.h"
#include "component/ScriptComponent.h"
#include "component/ButtonComponent.h"
#include "component/UIComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/InputFieldComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ActionComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/AnimatorComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/TexturePool.h"
#include "device/Device.h"
#include "device/QualityResolver.h"
#include "pool/SoundPool.h"
#include "pool/TextureDataPool.h"
#include "pool/ShaderPool.h"
#include "pool/FontPool.h"
#include "pool/ModelPool.h"
#include "pool/MeshLodPool.h"
#include "pool/MeshBufferPool.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "pool/ShakePresetPool.h"
#include "pool/PhysicsMaterialPool.h"
#include "pool/VertexAnimationPool.h"
#include "shader/RuntimeShaderCompiler.h"
#include "native/NativeBridge.h"
#include "net/Http.h"
#include "media/MediaCache.h"
#include "thread/MainThreadQueue.h"
#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "model/ModelCpuData.h"
#include "picking/PickCache.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"
#if JASPERENGINE_PROFILER
#include "profiler/EngineProfiling.h"
#endif

#include "sokol_time.h"
#include "PlatformConfig.h"
#include "player/RunControl.h"
#include "render/FrameReadback.h"
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif
#if defined(__ANDROID__)
    #include <sys/system_properties.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <functional>
#include <optional>

using namespace jasperengine;

// thread_local cannot be a data member of a DLL-exported class (MSVC C2492).
// Keep the TLS counter as file-local storage and expose it via a plain function.
static unsigned int& getAsyncThreadDepthStorage() {
    thread_local unsigned int asyncThreadDepth = 0;
    return asyncThreadDepth;
}

//-----JasperEngine user config-----
std::vector<Scene*> Engine::scenes;
std::unordered_set<Scene*> Engine::oneTimeScenes;
std::mutex Engine::sceneListMutex;

Scene* Engine::mainScene = NULL;

int Engine::canvasWidth;
int Engine::canvasHeight;

int Engine::preferredCanvasWidth;
int Engine::preferredCanvasHeight;

Rect Engine::viewRect;

Scaling Engine::scalingMode = Scaling::FITWIDTH;
AtmosphereQuality Engine::atmosphereQualityDefaults[3] = {AtmosphereQuality::HIGH, AtmosphereQuality::LOW, AtmosphereQuality::LOW};
bool Engine::atmosphereQualityOverridden = false;
AtmosphereQuality Engine::atmosphereQualityOverride = AtmosphereQuality::HIGH;
float Engine::foliageDistanceScaleDefaults[3] = {1.0f, 1.0f, 1.0f};
bool Engine::foliageDistanceScaleOverridden = false;
float Engine::foliageDistanceScaleOverride = 1.0f;
bool Engine::gpuInstancingEnabled = true;
CanvasScalerSettings Engine::defaultCanvasScaler;
TextureStrategy Engine::textureStrategy = TextureStrategy::NONE;

bool Engine::callMouseInTouchEvent = false;
bool Engine::callTouchInMouseEvent = false;
bool Engine::useDegrees = true;
bool Engine::allowEventsOutCanvas = false;
bool Engine::ignoreEventsHandledByUI = true;

bool Engine::uiEventReceived = false;

uint64_t Engine::lastTime = 0;
double Engine::framePeriod = 0.0;
double Engine::frameCpuTime = 0.0;
double Engine::frameDrawCpuTime = 0.0;
double Engine::updateTimeCount = 0;
int Engine::fixedStepsLeft = 1;

double Engine::deltatime = 0;
float Engine::framerate = 0;
Engine::FrameStats Engine::frameStats;
Engine::FrameStats Engine::currentFrameStats;

// Upper bound for the delta time exposed to gameplay and the update loop.
// A frame that stalls (scene load, debugger break, alt-tab) is clamped so scripts
// and physics never receive a giant step that teleports objects or triggers the
// spiral of death.
double Engine::maxDeltatime = 0.25;

double Engine::updateTime = 1.0 / 60.0; //60Hz

double Engine::timeScale = 1.0;
double Engine::fixedDeltatime = 0.0;
double Engine::gameTime = 0.0;
bool Engine::captureFrame = false;
double Engine::shaderTime = 0.0;
bool Engine::shaderTimeFixed = false;
double Engine::pinnedShaderTime = -1.0;
std::atomic<uint64_t> Engine::gameFrames{0};
// the clocks of Time (EngineTime.h): the unscaled frame delta and its sum, the fixed steps' sum,
// the game time at the last scene load, the wall clock at start
double Engine::unscaledDeltatime = 0.0;
double Engine::unscaledTime = 0.0;
double Engine::fixedTime = 0.0;
double Engine::levelLoadGameTime = 0.0;
uint64_t Engine::startupTime = 0;
bool Engine::inFixedStep = false;
float Engine::smoothDeltatime = 0.0f;
std::atomic<bool> Engine::simulationHeld = false;
bool Engine::clampNextDelta = false;

std::atomic<bool> Engine::viewLoaded = false;
std::atomic<bool> Engine::paused = false;
bool Engine::gameEventsPaused = false;
bool Engine::osPaused = false;
bool Engine::nativePaused = false;
std::atomic<bool> Engine::focused = true;
bool Engine::dynamicBatching = true;
bool Engine::dynamicBatchingDefault = true;
std::atomic<bool> Engine::visible = true;
int Engine::targetFrameRate = -1;
int Engine::textureQuality = 0;
uint32_t Engine::qualityRevision = 0;
// the project's VSync setting (platformConfig().vsync, set in systemInit; the generated init()
// may set it again)
int Engine::vSyncCount = 1;
FramePacing Engine::framePacing;
int Engine::lastOrientation = 0;
double Engine::orientationPendingSince = 0.0;
std::atomic<bool> Engine::asyncLoading = false;
bool Engine::frameRunning = false;
bool Engine::developmentBuild = false;

CursorType Engine::mouseCursorType = CursorType::ARROW;
MouseMode Engine::mouseMode = MouseMode::NORMAL;
bool Engine::windowFocused = true;

Semaphore Engine::drawSemaphore;

Framebuffer* Engine::framebuffer = nullptr;
Framebuffer Engine::compositeFramebuffer;

//-----JasperEngine user events-----
FunctionSubscribe<void()> Engine::onViewLoaded;
FunctionSubscribe<void()> Engine::onViewChanged;
FunctionSubscribe<void()> Engine::onViewDestroyed;
FunctionSubscribe<void()> Engine::onDraw;
FunctionSubscribe<void()> Engine::onUpdate;
FunctionSubscribe<void()> Engine::onFixedUpdate;
FunctionSubscribe<void()> Engine::onPostUpdate;
FunctionSubscribe<void()> Engine::onPause;
FunctionSubscribe<void()> Engine::onResume;
FunctionSubscribe<void()> Engine::onShutdown;
FunctionSubscribe<void(bool)> Engine::onFocus;
FunctionSubscribe<void(bool)> Engine::onVisibilityChanged;
FunctionSubscribe<void(int)> Engine::onOrientationChanged;
FunctionSubscribe<void()> Engine::onLowMemory;
FunctionSubscribe<void()> Engine::onSafeAreaChanged;
FunctionSubscribe<void()> Engine::onSceneLoaded;
FunctionSubscribe<void(int,float,float)> Engine::onTouchStart;
FunctionSubscribe<void(int,float,float)> Engine::onTouchEnd;
FunctionSubscribe<void(int,float,float)> Engine::onTouchMove;
FunctionSubscribe<void(int,float,float)> Engine::onTouchCancel;
FunctionSubscribe<void(int,float,float,int)> Engine::onMouseDown;
FunctionSubscribe<void(int,float,float,int)> Engine::onMouseUp;
FunctionSubscribe<void(float,float,int)> Engine::onMouseScroll;
FunctionSubscribe<void(float,float,int)> Engine::onMouseMove;
FunctionSubscribe<void()> Engine::onMouseEnter;
FunctionSubscribe<void()> Engine::onMouseLeave;
FunctionSubscribe<void(int,bool,int)> Engine::onKeyDown;
FunctionSubscribe<void(int,bool,int)> Engine::onKeyUp;
FunctionSubscribe<void(char32_t)> Engine::onCharInput;
FunctionSubscribe<void(int)> Engine::onGamepadConnect;
FunctionSubscribe<void(int)> Engine::onGamepadDisconnect;
FunctionSubscribe<void(int,int)> Engine::onGamepadButtonDown;
FunctionSubscribe<void(int,int)> Engine::onGamepadButtonUp;
FunctionSubscribe<void(int,int,float)> Engine::onGamepadAxisMove;
FunctionSubscribe<void(const std::string&)> Engine::onButtonDown;
FunctionSubscribe<void(const std::string&)> Engine::onButtonUp;

FunctionSubscribe<void()>& Engine::getOnInit() {
    // This static variable is guaranteed to be created the first time this function is called
    static FunctionSubscribe<void()> onInit;
    return onInit;
}

void Engine::setScene(Scene* scene){
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        if (mainScene){
            scenes.erase(std::remove(scenes.begin(), scenes.end(), mainScene), scenes.end());
        }

        if (scene){
            // a scene already registered as a layer would be updated twice per frame
            scenes.erase(std::remove(scenes.begin(), scenes.end(), scene), scenes.end());

            //main scene is allways first scene
            scenes.insert(scenes.begin(), scene);
            mainScene = scene;
        }
    }

    // includeScene() runs scene->load(), which can re-enter the scene list, so it stays
    // outside sceneListMutex. Same reasoning in addSceneLayer and executeSceneOnce.
    if (scene)
        includeScene(scene);

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

Scene* Engine::getScene(){
    std::lock_guard<std::mutex> lock(sceneListMutex);
    // none before the first scene is set (a scene's scripts start before it is); never the
    // persistent scene (DontDestroyOnLoad), first while a load builds its new scenes
    Scene* persistent = SceneManager::getPersistentScene();
    for (Scene* scene : Engine::scenes) {
        if (scene != persistent) {
            return scene;
        }
    }
    return nullptr;
}

std::vector<Scene*> Engine::getScenesSnapshot(){
    std::lock_guard<std::mutex> lock(sceneListMutex);
    return scenes;
}

std::vector<Scene*> Engine::getSceneLayers(){
    return getScenesSnapshot();
}

void Engine::addSceneLayer(Scene* scene){
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    bool linked = false;

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        auto it = std::find(scenes.begin(), scenes.end(), scene);
        if (scene && (it == scenes.end())) {
            scenes.push_back(scene);
            linked = true;
        }
    }

    if (linked)
        includeScene(scene);

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

void Engine::executeSceneOnce(Scene* scene) {
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    bool linked = false;

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        if (scene) {
            oneTimeScenes.insert(scene);

            auto it = std::find(scenes.begin(), scenes.end(), scene);
            if (it == scenes.end()) {
                scenes.push_back(scene);
                linked = true;
            }
        }
    }

    if (linked)
        includeScene(scene);

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

void Engine::removeScene(Scene* scene){
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    bool unlinked = false;

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        if (scene){
            auto it = std::find(scenes.begin(), scenes.end(), scene);

            if (it != scenes.end()) {
                scenes.erase(it);
                unlinked = true;
            }

            if (mainScene == scene){
                mainScene = NULL;
            }

            // A scene removed explicitly must not remain in the one-shot registry.
            // Callers may destroy it immediately after this function returns.
            oneTimeScenes.erase(scene);
        }
    }

    if (unlinked)
        scene->getSystem<UISystem>()->resetButtonStates();

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

void Engine::removeAllSceneLayers(bool removeOneTimeScenes){
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        scenes.erase(
            std::remove_if(scenes.begin(), scenes.end(),
                [removeOneTimeScenes](Scene* scene) {
                    // Keep main scene
                    if (scene == mainScene)
                        return false;

                    // If removeOneTimeScenes is false, keep scenes in oneTimeScenes
                    if (!removeOneTimeScenes && oneTimeScenes.find(scene) != oneTimeScenes.end())
                        return false;

                    // Remove all other scenes
                    return true;
                }),
            scenes.end()
        );

        if (removeOneTimeScenes){
            // A one-shot dropped from the draw list can never finish loading, so leaving it
            // registered would keep hasScenesToExecuteOnce() true forever.
            for (auto it = oneTimeScenes.begin(); it != oneTimeScenes.end(); ){
                if (*it != mainScene){
                    it = oneTimeScenes.erase(it);
                }else{
                    ++it;
                }
            }
        }
    }

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

void Engine::removeAllScenes(){
    if (Engine::isAsyncThread())
        drawSemaphore.acquire();

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);

        scenes.clear();
        mainScene = NULL;
        oneTimeScenes.clear();
    }

    if (Engine::isAsyncThread())
        drawSemaphore.release();
}

bool Engine::isSceneRunning(Scene* scene){
    if (!scene) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sceneListMutex);
    auto it = std::find(scenes.begin(), scenes.end(), scene);
    return it != scenes.end();
}

bool Engine::hasScenesToExecuteOnce(){
    std::lock_guard<std::mutex> lock(sceneListMutex);
    return !oneTimeScenes.empty();
}

Scene* Engine::getMainScene(){
    return mainScene;
}

Scene* Engine::getBaseScene(){
    return mainScene ? mainScene : getScene();
}

Scene* Engine::getLastScene(){
    std::lock_guard<std::mutex> lock(sceneListMutex);
    return scenes.empty() ? nullptr : scenes.back();
}

bool Engine::advanceGameClocks(Scheduler& scheduler, double frameTime) {
    if (paused || simulationHeld || gameEventsPaused) return false;
    scheduler.advance(TimeDomain::Game, frameTime * timeScale);
    scheduler.advance(TimeDomain::Unscaled, frameTime);
    scheduler.advance(TimeDomain::Frames, 1.0);
    return true;
}

bool Engine::areGameEventsPaused() {
    return gameEventsPaused;
}

void Engine::pauseGameEvents(bool pause) {
    gameEventsPaused = pause;
    // Pause ALL subscriptions for game events
    // Don't pause drawing and lifecycle events
    onUpdate.setEnabled(!pause);
    onFixedUpdate.setEnabled(!pause);
    onPostUpdate.setEnabled(!pause);
    onTouchStart.setEnabled(!pause);
    onTouchEnd.setEnabled(!pause);
    onTouchMove.setEnabled(!pause);
    onTouchCancel.setEnabled(!pause);
    onMouseDown.setEnabled(!pause);
    onMouseUp.setEnabled(!pause);
    onMouseMove.setEnabled(!pause);
    onMouseScroll.setEnabled(!pause);
    onKeyDown.setEnabled(!pause);
    onKeyUp.setEnabled(!pause);
    onCharInput.setEnabled(!pause);
    onGamepadButtonDown.setEnabled(!pause);
    onGamepadButtonUp.setEnabled(!pause);
    onGamepadAxisMove.setEnabled(!pause);
    onButtonDown.setEnabled(!pause);
    onButtonUp.setEnabled(!pause);
}

void Engine::includeScene(Scene* scene){
    if (viewLoaded){
        scene->load();
        scene->updateCameraSize();
    }
}

int Engine::getCanvasWidth(){
    return Engine::canvasWidth;
}

int Engine::getCanvasHeight(){
    return Engine::canvasHeight;
}

void Engine::setCanvasSize(int canvasWidth, int canvasHeight){
    Engine::preferredCanvasWidth = canvasWidth;
    Engine::preferredCanvasHeight = canvasHeight;

    if (viewLoaded){
        // at runtime (Lua Engine.setCanvasSize): the cameras follow now, not at the next resize
        refreshCanvas(true);
    }else{
        // Scenes and scripts can start before the view exists; the screen refines this once it loads
        Engine::canvasWidth = canvasWidth;
        Engine::canvasHeight = canvasHeight;
    }
}

int Engine::getPreferredCanvasWidth(){
    return Engine::preferredCanvasWidth;
}

int Engine::getPreferredCanvasHeight(){
    return Engine::preferredCanvasHeight;
}

Rect Engine::getViewRect(){
    return viewRect;
}

void Engine::setScalingMode(Scaling scalingMode){
    Engine::scalingMode = scalingMode;

    if (viewLoaded){
        refreshCanvas(true);
    }
}

Scaling Engine::getScalingMode(){
    return scalingMode;
}

void Engine::setDefaultCanvasScaler(const CanvasScalerSettings& settings){
    Engine::defaultCanvasScaler = settings;

    if (viewLoaded){
        for (Scene* scene : getScenesSnapshot()){
            scene->updateCameraSize();
        }
    }
}

const CanvasScalerSettings& Engine::getDefaultCanvasScaler(){
    return defaultCanvasScaler;
}

void Engine::setTextureStrategy(TextureStrategy textureStrategy){
    Engine::textureStrategy = textureStrategy;
}

TextureStrategy Engine::getTextureStrategy(){
    return textureStrategy;
}

void Engine::setTextureQuality(int quality){
    textureQuality = std::clamp(quality, 0, 3);
    // what the pool drops: the device tier and memory pressure may drop more (QualityResolver)
    TextureDataPool::setTextureQuality(QualityResolver::textureQuality(textureQuality));
}

int Engine::getTextureQuality(){
    return textureQuality;
}

void Engine::setTextureMemoryBudget(int megabytes){
    TextureDataPool::setMemoryBudget(megabytes > 0 ? (uint64_t)megabytes * 1024ull * 1024ull : 0);
}

int Engine::getTextureMemoryBudget(){
    return (int)(TextureDataPool::getMemoryBudget() / (1024ull * 1024ull));
}

uint64_t Engine::getTextureMemoryUsage(){
    return TextureDataPool::getResidentBytes();
}

void Engine::setTextureLowMemoryFallback(bool enabled){
    TextureDataPool::setLowMemoryFallback(enabled);
}

bool Engine::isTextureLowMemoryFallback(){
    return TextureDataPool::getLowMemoryFallback();
}

bool Engine::isTextureFormatSupported(const std::string& format){
    GpuTextureFormat parsed;
    return gpuFormatFromName(format, parsed) && SystemRender::isTextureFormatSupported(parsed);
}

void Engine::setTextureDefaults(const TexturePlatformDefaults& desktop, const TexturePlatformDefaults& mobile,
                                const TexturePlatformDefaults& web){
    const Platform platform = getPlatform();
    const TexturePlatformDefaults& chosen = (platform == Platform::iOS || platform == Platform::Android) ? mobile
                                          : platform == Platform::Web ? web : desktop;
    setTextureQuality(chosen.quality);
    setTextureMemoryBudget(chosen.memoryBudgetMB);
    setTextureLowMemoryFallback(chosen.lowMemoryFallback);
}

void Engine::setCallMouseInTouchEvent(bool callMouseInTouchEvent){
    Engine::callMouseInTouchEvent = callMouseInTouchEvent;
}

bool Engine::isCallMouseInTouchEvent(){
    return Engine::callMouseInTouchEvent;
}

void Engine::setCallTouchInMouseEvent(bool callTouchInMouseEvent){
    Engine::callTouchInMouseEvent = callTouchInMouseEvent;
}

bool Engine::isCallTouchInMouseEvent(){
    return Engine::callTouchInMouseEvent;
}

void Engine::setUseDegrees(bool useDegrees){
    Engine::useDegrees = useDegrees;
}

bool Engine::isUseDegrees(){
    return Engine::useDegrees;
}

void Engine::setAllowEventsOutCanvas(bool allowEventsOutCanvas){
    Engine::allowEventsOutCanvas = allowEventsOutCanvas;
}

bool Engine::isAllowEventsOutCanvas(){
    return allowEventsOutCanvas;
}

void Engine::setMeshRaycastMaxTriangles(unsigned int maxTriangles){
    PickCache::setMaxTriangles(maxTriangles);
}

unsigned int Engine::getMeshRaycastMaxTriangles(){
    return PickCache::getMaxTriangles();
}

void Engine::setIgnoreEventsHandledByUI(bool ignoreEventsHandledByUI){
    Engine::ignoreEventsHandledByUI = ignoreEventsHandledByUI;
}

bool Engine::isIgnoreEventsHandledByUI(){
    return ignoreEventsHandledByUI;
}

bool Engine::isUIEventReceived(){
    return uiEventReceived;
}

bool Engine::isPointerOverUI(int pointerId){
    for (Scene* scene : getScenesSnapshot()){
        if (!scene->canReceiveUIEvents())
            continue;
        if (scene->getSystem<UISystem>()->isPointerOverUI(pointerId))
            return true;
        if (scene->getSystem<UIDocumentSystem>()->isPointerOverUI(pointerId))
            return true;
    }
    return false;
}

void Engine::setUpdateTimeMS(unsigned int updateTimeMS){
    if (updateTimeMS == 0){
        Log::warn("Engine::setUpdateTimeMS: zero is invalid, ignoring");
        return;
    }
    Engine::updateTime = updateTimeMS / 1000.0;
}

void Engine::setUpdateTime(float updateTime){
    if (!(updateTime > 0.0f) || std::isnan(updateTime) || std::isinf(updateTime)){
        Log::warn("Engine::setUpdateTime: non-positive or non-finite value (%f) ignored", updateTime);
        return;
    }
    Engine::updateTime = updateTime;
}

float Engine::getUpdateTime(){
    return (float)Engine::updateTime;
}

int Engine::getFixedStepsLeft(){
    return fixedStepsLeft;
}

double Engine::getInterpolationAlpha(){
    if (updateTime <= 0.0) return 0.0;
    double alpha = updateTimeCount / updateTime;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    return alpha;
}

void Engine::setMouseCursor(CursorType type){
    if (viewLoaded){
        System::instance().setMouseCursor(type);
    }
    mouseCursorType = type;
}

CursorType Engine::getMouseCursor(){
    return mouseCursorType;
}

void Engine::setMouseMode(MouseMode mode){
    Engine::mouseMode = mode;
    // a browser locks the pointer at the next gesture (systemPointerLockChanged), a
    // desktop window at once
    if (mode != MouseMode::CAPTURED){
        Input::setPointerLockState(PointerLockState::UNLOCKED);
    }else{
#ifdef __EMSCRIPTEN__
        if (Input::getPointerLockState() != PointerLockState::LOCKED)
            Input::setPointerLockState(PointerLockState::PENDING);
#else
        Input::setPointerLockState(windowFocused ? PointerLockState::LOCKED : PointerLockState::PENDING);
#endif
    }
    if (viewLoaded){
        System::instance().setMouseMode(mode);
    }
}

void Engine::systemPointerLockChanged(bool locked){
    if (mouseMode != MouseMode::CAPTURED){
        Input::setPointerLockState(PointerLockState::UNLOCKED);
        return;
    }
    Input::setPointerLockState(locked ? PointerLockState::LOCKED : PointerLockState::PENDING);
}

void Engine::systemFocusChanged(bool focused){
    windowFocused = focused;
    if (focused){
#ifndef __EMSCRIPTEN__
        if (mouseMode == MouseMode::CAPTURED)
            Input::setPointerLockState(PointerLockState::LOCKED);
#endif
        return;
    }

    // what is held comes up first, through the events (the documents and onKeyUp hear it,
    // as PlayInput::releaseAll does in the editor), so nothing stays pressed while another
    // window has the keyboard; the fingers are cancelled
    for (int key = 0; key <= D_KEY_LAST; key++){
        if (Input::getKey(key))
            systemKeyUp(key, false, 0);
    }
    const Vector2 mouse = Input::getMousePosition();
    float screenX = viewRect.getX();
    float screenY = viewRect.getY();
    if (canvasWidth != 0)
        screenX += (mouse.x / (float)canvasWidth) * viewRect.getWidth();
    if (canvasHeight != 0)
        screenY += (mouse.y / (float)canvasHeight) * viewRect.getHeight();
    for (int button = 0; button <= D_MOUSE_BUTTON_LAST; button++){
        if (Input::getMouseButton(button)){
            systemMouseUp(button, screenX, screenY, 0);
            // released out of the canvas: transformCoordPos dropped the event
            if (Input::getMouseButton(button))
                Input::releaseMousePressed(button);
        }
    }
    systemTouchCancel();
    // what no event released (a gamepad's buttons, a key or button the events left down) comes
    // up too, without events
    Input::releaseAll();
    Input::setModifiers(0);
    Input::clearPendingDeltas();
    if (mouseMode == MouseMode::CAPTURED)
        Input::setPointerLockState(PointerLockState::PENDING);
}

bool Engine::isWindowFocused(){
    return windowFocused;
}

MouseMode Engine::getMouseMode(){
    return mouseMode;
}

void Engine::setMousePosition(float x, float y){
    // a warp is no motion: the next move's delta starts here
    Input::warpMouse(x, y);

    if (!viewLoaded)
        return;

    float screenX = viewRect.getX();
    float screenY = viewRect.getY();
    if (canvasWidth != 0)
        screenX += (x / (float)canvasWidth) * viewRect.getWidth();
    if (canvasHeight != 0)
        screenY += (y / (float)canvasHeight) * viewRect.getHeight();

    System::instance().setMousePosition(screenX, screenY);
}

Platform Engine::getPlatform(){
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    return Platform::Windows;
#elif __APPLE__
    #include <TargetConditionals.h>
    #if TARGET_OS_SIMULATOR
    return Platform::iOS;
    #elif TARGET_OS_IPHONE
    return Platform::iOS;
    #elif TARGET_OS_MAC
    return Platform::MacOS;
    #endif
#elif __ANDROID__
    return Platform::Android;
#elif __EMSCRIPTEN__
    return Platform::Web;
#elif __linux__
    return Platform::Linux;
#endif
}

void Engine::setGpuInstancingEnabled(bool enabled){
    gpuInstancingEnabled = enabled;
}

bool Engine::isGpuInstancingEnabled(){
    return gpuInstancingEnabled;
}

void Engine::setAtmosphereQualityDefaults(AtmosphereQuality desktop, AtmosphereQuality mobile, AtmosphereQuality web){
    atmosphereQualityDefaults[0] = desktop;
    atmosphereQualityDefaults[1] = mobile;
    atmosphereQualityDefaults[2] = web;
}

AtmosphereQuality Engine::getAtmosphereQualityDefault(Platform platform){
    switch (platform){
        case Platform::iOS:
        case Platform::Android:
            return atmosphereQualityDefaults[1];
        case Platform::Web:
            return atmosphereQualityDefaults[2];
        default:
            return atmosphereQualityDefaults[0];
    }
}

AtmosphereQuality Engine::getAtmosphereQualityDefault(){
    return getAtmosphereQualityDefault(getPlatform());
}

void Engine::setAtmosphereQuality(AtmosphereQuality quality){
    atmosphereQualityOverride = quality;
    atmosphereQualityOverridden = true;
}

AtmosphereQuality Engine::getAtmosphereQuality(){
    return atmosphereQualityOverridden ? atmosphereQualityOverride : getAtmosphereQualityDefault();
}

void Engine::resetAtmosphereQuality(){
    atmosphereQualityOverridden = false;
}

bool Engine::isAtmosphereQualityOverridden(){
    return atmosphereQualityOverridden;
}

namespace {
    // the Foliage Distance range: 0 draws none, 10 is ten times the authored distances
    float clampFoliageDistanceScale(float scale){
        return std::clamp(scale, 0.0f, 10.0f);
    }
}

void Engine::setFoliageDistanceScaleDefaults(float desktop, float mobile, float web){
    const float scales[3] = {desktop, mobile, web};
    for (int i = 0; i < 3; i++){
        foliageDistanceScaleDefaults[i] = std::isfinite(scales[i]) ? clampFoliageDistanceScale(scales[i]) : 1.0f;
    }
}

float Engine::getFoliageDistanceScaleDefault(Platform platform){
    switch (platform){
        case Platform::iOS:
        case Platform::Android:
            return foliageDistanceScaleDefaults[1];
        case Platform::Web:
            return foliageDistanceScaleDefaults[2];
        default:
            return foliageDistanceScaleDefaults[0];
    }
}

float Engine::getFoliageDistanceScaleDefault(){
    return getFoliageDistanceScaleDefault(getPlatform());
}

void Engine::setFoliageDistanceScale(float scale){
    if (!std::isfinite(scale)){
        Log::warn("Engine::setFoliageDistanceScale: non-finite value ignored");
        return;
    }
    foliageDistanceScaleOverride = clampFoliageDistanceScale(scale);
    foliageDistanceScaleOverridden = true;
}

float Engine::getFoliageDistanceScale(){
    return foliageDistanceScaleOverridden ? foliageDistanceScaleOverride : getFoliageDistanceScaleDefault();
}

void Engine::resetFoliageDistanceScale(){
    foliageDistanceScaleOverridden = false;
}

GraphicBackend Engine::getGraphicBackend(){
#if defined(SOKOL_GLCORE)
    return GraphicBackend::GLCORE;
#elif defined(SOKOL_GLES3)
    return GraphicBackend::GLES3;
#elif defined(SOKOL_D3D11)
    return GraphicBackend::D3D11;
#elif defined(SOKOL_METAL)
    return GraphicBackend::METAL;
#elif defined(SOKOL_WGPU)
    return GraphicBackend::WGPU;
#elif defined(SOKOL_VULKAN)
    return GraphicBackend::VULKAN;
#elif defined(JASPERENGINE_APPLE) //Xcode template
    return GraphicBackend::METAL;
#endif
}

bool Engine::isOpenGL(){
    GraphicBackend gbackend = getGraphicBackend();

    if (gbackend == GraphicBackend::GLCORE)
        return true;
    if (gbackend == GraphicBackend::GLES3)
        return true;

    return false;
}

float Engine::getFramerate(){
    return framerate;
}

double Engine::getFramePeriod(){
    return framePeriod;
}

double Engine::getFrameDrawCpuTime(){
    return frameDrawCpuTime;
}

double Engine::getFrameCpuTime(){
    return frameCpuTime;
}

float Engine::getDeltatime(){
    return deltatime;
}

void Engine::beginFrameStats(){
    frameStats = currentFrameStats;
    currentFrameStats = FrameStats();
}

uint32_t Engine::getFrameDrawCalls(){
    return currentFrameStats.drawCalls;
}

void Engine::addDrawStats(uint32_t instances, uint64_t triangles, uint64_t vertices){
    currentFrameStats.drawCalls++;
    currentFrameStats.instances += instances;
    currentFrameStats.triangles += triangles;
    currentFrameStats.vertices += vertices;
}

void Engine::addVisibilityStats(uint32_t visible, uint32_t culled){
    currentFrameStats.visible += visible;
    currentFrameStats.culled += culled;
}

uint32_t Engine::getPendingDrawCalls(){
    return currentFrameStats.drawCalls;
}

const Engine::FrameStats& Engine::getFrameStats(){
    return frameStats;
}

float Engine::getMaxDeltatime(){
    return (float)maxDeltatime;
}

void Engine::setMaxDeltatime(float seconds){
    if (!(seconds > 0.0f) || std::isinf(seconds)){
        Log::warn("Engine::setMaxDeltatime: non-positive or non-finite value (%f) ignored", seconds);
        return;
    }
    maxDeltatime = seconds;
}

float Engine::getUnscaledDeltatime(){
    return (float)unscaledDeltatime;
}

double Engine::getUnscaledTime(){
    return unscaledTime;
}

double Engine::getFixedTime(){
    return fixedTime;
}

double Engine::getRealtimeSinceStartup(){
    return stm_sec(stm_since(startupTime));
}

double Engine::getTimeSinceLevelLoad(){
    return gameTime - levelLoadGameTime;
}

void Engine::markLevelLoad(){
    levelLoadGameTime = gameTime;
}

bool Engine::isInFixedStep(){
    return inFixedStep;
}

float Engine::getSmoothDeltatime(){
    return smoothDeltatime;
}

double Engine::getSystemTime(){
    return stm_sec(stm_now());
}

void Engine::latchShaderTime(){
    // in fixed mode: the clock where fixed mode began (or the delta changed), plus whole frames
    // of the delta, so no rounding piles up over a long capture
    static double fixedBase = 0.0;
    static double fixedDelta = 0.0;
    static uint64_t fixedFrames = 0;
    if (pinnedShaderTime >= 0.0){
        shaderTime = pinnedShaderTime;
        shaderTimeFixed = false;
    }else if (fixedDeltatime > 0.0){
        if (!shaderTimeFixed || fixedDelta != fixedDeltatime){
            fixedBase = shaderTime;
            fixedDelta = fixedDeltatime;
            fixedFrames = 0;
        }else{
            fixedFrames++;
        }
        shaderTimeFixed = true;
        shaderTime = fixedBase + (double)fixedFrames * fixedDelta;
    }else{
        shaderTimeFixed = false;
        shaderTime = getSystemTime();
    }
}

void Engine::setPinnedShaderTime(double seconds){
    if (std::isnan(seconds) || std::isinf(seconds)){
        Log::warn("Engine::setPinnedShaderTime: non-finite value ignored");
        return;
    }
    pinnedShaderTime = seconds < 0.0 ? -1.0 : seconds;
    if (pinnedShaderTime >= 0.0){
        shaderTime = pinnedShaderTime;
        shaderTimeFixed = false;
    }
}

bool Engine::hasPinnedShaderTime(){
    return pinnedShaderTime >= 0.0;
}

double Engine::getPinnedShaderTime(){
    return pinnedShaderTime;
}

float Engine::getShaderTime(){
    return (float)shaderTime;
}

double Engine::getShaderTimeSeconds(){
    return shaderTime;
}

void Engine::setTimeScale(float scale){
    if (std::isnan(scale) || std::isinf(scale)){
        Log::warn("Engine::setTimeScale: non-finite value ignored");
        return;
    }
    timeScale = (scale < 0.0f) ? 0.0 : (scale > 100.0f) ? 100.0 : (double)scale;
}

float Engine::getTimeScale(){
    return (float)timeScale;
}

void Engine::setFixedDeltatime(double seconds){
    if (std::isnan(seconds) || std::isinf(seconds) || seconds < 0.0){
        Log::warn("Engine::setFixedDeltatime: negative or non-finite value (%f) ignored", seconds);
        return;
    }
    fixedDeltatime = seconds;
}

double Engine::getFixedDeltatime(){
    return fixedDeltatime;
}

void Engine::setDevelopmentBuild(bool development){
    developmentBuild = development;
}

bool Engine::isDevelopmentBuild(){
    return developmentBuild;
}

double Engine::getGameTime(){
    return gameTime;
}

uint64_t Engine::getGameFrames(){
    return gameFrames.load(std::memory_order_relaxed);
}

void Engine::setSimulationHeld(bool held){
    if (simulationHeld && !held){
        clampNextDelta = true;
    }
    simulationHeld = held;
}

bool Engine::isSimulationHeld(){
    return simulationHeld;
}

// Honoured everywhere: without threads (single-thread web) the job system runs the loads
// from its per-frame pump, so they still complete over several frames.
void Engine::setAsyncLoading(bool enable){
    asyncLoading = enable;
}

bool Engine::isAsyncLoading(){
    return asyncLoading;
}

void Engine::updateGpuCreation(){
    // under the frames' semaphore: a thread loading a scene asks for the states this publishes
    drawSemaphore.acquire();
    SystemRender::updateGpuCreation();
    ShaderWarmup::update();
    drawSemaphore.release();
}

void Engine::startAsyncThread(){
    #ifndef NO_THREAD_SUPPORT
        getAsyncThreadDepthStorage()++;
    #else
        Log::warn("Threads are not available");
    #endif
}

void Engine::commitThreadQueue(){
    SystemRender::commitQueue();
}

void Engine::endAsyncThread(){
    #ifndef NO_THREAD_SUPPORT
        unsigned int& asyncThreadDepth = getAsyncThreadDepthStorage();
        if (asyncThreadDepth == 0){
            Log::warn("Engine::endAsyncThread called without matching startAsyncThread");
            return;
        }

        asyncThreadDepth--;
        if (asyncThreadDepth == 0){
            commitThreadQueue();
        }
    #endif
}

bool Engine::isAsyncThread(){
    return getAsyncThreadDepthStorage() > 0;
}

bool Engine::isFrameRunning(){
    return frameRunning;
}

bool Engine::isViewLoaded(){
    return viewLoaded;
}

void Engine::setMaxResourceLoadingThreads(size_t maxThreads) {
    // live: the gate's counters change, the pool is never rebuilt
    jobs::system().getGate().setMaxOverride((int)std::min<size_t>(maxThreads, 64));
}

size_t Engine::getQueuedResourceCount() {
    if (!jobs::hasSystem())
        return 0;
    return jobs::system().getAssetJobCount();
}

namespace{
    ThreadingSettings& threadingSettingsStorage(){
        static ThreadingSettings settings;
        return settings;
    }

    jobs::Config jobConfigFor(const ThreadingSettings& settings){
        jobs::Config config;
        config.workers = settings.workers;
        config.reserveForFrame = settings.reservedFrameWorkers;
        return config;
    }

    // the job system and the asset readers, with the settings known now
    void startJobs(){
        const ThreadingSettings& settings = threadingSettingsStorage();
        jobs::initialize(jobConfigFor(settings));
        if (!AssetIOService::instance().isRunning())
            AssetIOService::instance().start(settings.ioThreads);
    }
}

void Engine::setThreadingSettings(const ThreadingSettings& settings) {
    threadingSettingsStorage() = settings;
    // a running system takes the reserve live (initialize) and warns about the worker count
    startJobs();
}

const ThreadingSettings& Engine::getThreadingSettings() {
    return threadingSettingsStorage();
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace{
    // Android, iOS and the web: sokol's per-frame buffers are host-visible memory shared with
    // the rest of a smaller device
    bool smallGpuMemoryPlatform(){
#if defined(JASPERENGINE_ANDROID) || defined(JASPERENGINE_WEB) || defined(__EMSCRIPTEN__)
        return true;
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
        return true;
    #else
        return false;
    #endif
#else
        return false;
#endif
    }
}

int Engine::defaultUniformBytes(){
    return (smallGpuMemoryPlatform() ? 16 : 64) * 1024 * 1024;
}

int Engine::defaultStreamStagingBytes(){
    return (smallGpuMemoryPlatform() ? 16 : 48) * 1024 * 1024;
}

int Engine::defaultTerrainUploadBytes(){
    return (smallGpuMemoryPlatform() ? 2 : 8) * 1024 * 1024;
}

int Engine::defaultDescriptorBytes(){
    return (smallGpuMemoryPlatform() ? 16 : 64) * 1024 * 1024;
}

namespace{
    Engine::GpuResourceLimits& gpuResourceLimits(){
        static Engine::GpuResourceLimits limits;
        return limits;
    }
}

void Engine::setGpuResourceLimits(const GpuResourceLimits& limits){
    const GpuResourceLimits defaults;
    // sokol's pools take 1 to 65,535 slots
    auto valid = [](int value, int fallback){ return (value > 0 && value < 65536) ? value : fallback; };
    GpuResourceLimits& set = gpuResourceLimits();
    set.buffers = valid(limits.buffers, defaults.buffers);
    set.images = valid(limits.images, defaults.images);
    set.samplers = valid(limits.samplers, defaults.samplers);
    set.views = valid(limits.views, defaults.views);
    set.pipelines = valid(limits.pipelines, defaults.pipelines);
    set.shaders = valid(limits.shaders, defaults.shaders);
    // sokol's per-frame buffers: 64 KB - 1 GB
    auto validBytes = [](int value, int fallback){ return (value >= 65536 && value <= (1 << 30)) ? value : fallback; };
    set.uniformBytes = validBytes(limits.uniformBytes, defaults.uniformBytes);
    set.streamStagingBytes = validBytes(limits.streamStagingBytes, defaults.streamStagingBytes);
    set.descriptorBytes = validBytes(limits.descriptorBytes, defaults.descriptorBytes);
    // sokol's WebGPU bind group cache: a power of 2, 2 - 32,768 (its pools stay under 65,536)
    const int cache = limits.bindGroupsCacheSize;
    set.bindGroupsCacheSize = (cache >= 2 && cache <= 32768 && (cache & (cache - 1)) == 0) ? cache : defaults.bindGroupsCacheSize;
}

const Engine::GpuResourceLimits& Engine::getGpuResourceLimits(){
    return gpuResourceLimits();
}

void Engine::setFramebuffer(Framebuffer* framebuffer){
    Engine::framebuffer = framebuffer;
}

Framebuffer* Engine::getFramebuffer(){
    return Engine::framebuffer;
}

bool Engine::ensureCompositeFramebuffer(){
    const unsigned int width = System::instance().getScreenWidth();
    const unsigned int height = System::instance().getScreenHeight();
    if (width == 0 || height == 0)
        return false;

    if (compositeFramebuffer.isCreated() && compositeFramebuffer.getWidth() == width
            && compositeFramebuffer.getHeight() == height)
        return true;

    compositeFramebuffer.destroy();
    compositeFramebuffer.setWidth(width);
    compositeFramebuffer.setHeight(height);
    compositeFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    compositeFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    {
        JE_GPU_ALLOC_SCOPE(RenderTargets, "Scene stack composite");
        compositeFramebuffer.create();
    }

    return compositeFramebuffer.isCreated();
}

void Engine::beginCompositeFramebuffer(){
// each scene opens its own swapchain pass and the sokol Vulkan backend takes only one
// per commit, so stacked scenes draw offscreen and are presented once. The other
// backends draw them straight to the swapchain (re-check when sokol_gfx is updated).
// A screenshot's capture frame draws offscreen on every backend, to be read after it.
#if defined(SOKOL_VULKAN)
    const bool stacked = scenes.size() > 1;
#else
    const bool stacked = false;
#endif
    // a run that writes a screenshot draws offscreen from its first frame, so the capture frame
    // needs no pipeline the frames before did not (formats of the window and the composite can
    // differ: a pipeline made on the capture frame would leave it empty). The debug overlay's
    // Scene is drawn as one more layer (while it draws anything).
    const bool screenshotRun = player::RunControl::isActive() && player::RunControl::wantsScreenshot();
    if (framebuffer || !(stacked || DebugOverlay::isDrawn() || captureFrame || screenshotRun))
        return;

    if (ensureCompositeFramebuffer())
        framebuffer = &compositeFramebuffer;
}

bool Engine::readCompositeFrame(std::vector<uint8_t>& rgba, int& width, int& height, std::string& error){
    if (!compositeFramebuffer.isCreated()){
        error = "the frame did not draw offscreen (no view yet)";
        return false;
    }
    width = static_cast<int>(compositeFramebuffer.getWidth());
    height = static_cast<int>(compositeFramebuffer.getHeight());
    return FrameReadback::readFramebuffer(compositeFramebuffer, rgba, &error);
}

bool Engine::isHeadless(){
    return player::RunControl::isHeadless();
}

void Engine::quit(int code){
    player::RunControl::requestQuit(code);
}


void Engine::endCompositeFramebuffer(){
    if (framebuffer != &compositeFramebuffer)
        return;

    framebuffer = nullptr;

    // the loading scene covers the start as a layer, before there is a main scene
    Scene* scene = getBaseScene();
    if (scene)
        scene->getSystem<RenderSystem>()->presentFramebufferToSwapchain(&compositeFramebuffer);
}

void Engine::clearPools(){
    // first: its assets' payload textures go back to the texture pools cleared next
    VertexAnimationPool::clear();
    // the global shader textures (Shader.setGlobalTexture) hold pool entries too
    ShaderGlobals::releaseTextures();
    TexturePool::clear();
    SoundPool::clear();
    TextureDataPool::clear();
    ShaderPool::clear();
    FontPool::clear();
    ModelPool::clear();
    MeshLodPool::clear();
    MeshBufferPool::clear();
    ModelCpuData::clear();
    AnimationClipPool::clear();
    AnimatorAssetPool::clear();
    ShakePresetPool::clear();
    PhysicsMaterialPool::clear();
}

void Engine::clearUnusedPools(){
    VertexAnimationPool::clearUnused();
    TexturePool::clearUnused();
    SoundPool::clearUnused();
    TextureDataPool::clearUnused();
    ShaderPool::clearUnused();
    FontPool::clearUnused();
    ModelPool::clearUnused();
    MeshLodPool::clearUnused();
    MeshBufferPool::clearUnused();
    AnimationClipPool::clearUnused();
    AnimatorAssetPool::clearUnused();
    PhysicsMaterialPool::clearUnused();
}

void Engine::removeSubscriptionsByTag(const std::string& substring) {
    onUpdate.removeByTagSubstring(substring);
    onFixedUpdate.removeByTagSubstring(substring);
    onPostUpdate.removeByTagSubstring(substring);

    onTouchStart.removeByTagSubstring(substring);
    onTouchEnd.removeByTagSubstring(substring);
    onTouchMove.removeByTagSubstring(substring);
    onTouchCancel.removeByTagSubstring(substring);

    onMouseDown.removeByTagSubstring(substring);
    onMouseUp.removeByTagSubstring(substring);
    onMouseScroll.removeByTagSubstring(substring);
    onMouseMove.removeByTagSubstring(substring);
    onMouseEnter.removeByTagSubstring(substring);
    onMouseLeave.removeByTagSubstring(substring);

    onKeyDown.removeByTagSubstring(substring);
    onKeyUp.removeByTagSubstring(substring);
    onCharInput.removeByTagSubstring(substring);

    onGamepadConnect.removeByTagSubstring(substring);
    onGamepadDisconnect.removeByTagSubstring(substring);
    onGamepadButtonDown.removeByTagSubstring(substring);
    onGamepadButtonUp.removeByTagSubstring(substring);
    onGamepadAxisMove.removeByTagSubstring(substring);
    onButtonDown.removeByTagSubstring(substring);
    onButtonUp.removeByTagSubstring(substring);

    onViewLoaded.removeByTagSubstring(substring);
    onViewChanged.removeByTagSubstring(substring);
    onViewDestroyed.removeByTagSubstring(substring);
    onDraw.removeByTagSubstring(substring);
    onPause.removeByTagSubstring(substring);
    onResume.removeByTagSubstring(substring);
    onShutdown.removeByTagSubstring(substring);
    onFocus.removeByTagSubstring(substring);
    onOrientationChanged.removeByTagSubstring(substring);
    onVisibilityChanged.removeByTagSubstring(substring);
    onLowMemory.removeByTagSubstring(substring);
    onSafeAreaChanged.removeByTagSubstring(substring);
    onSceneLoaded.removeByTagSubstring(substring);

    // Log.onMessageReceived and the console commands a released script added
    Log::onMessageReceived.removeByTagSubstring(substring);
    Device::onThermalStateChanged.removeByTagSubstring(substring);
    Device::onLowMemory.removeByTagSubstring(substring);
    DebugOverlay::removeCommandsByTag(substring);

    // a released script instance's tag is "_<address>_": the input sources it owned go too
    // (Input.simulateAxis(name, value, self), InputActions::setSourceValue(this, ...))
    if (substring.size() > 2 && substring.front() == '_' && substring.back() == '_' &&
            std::all_of(substring.begin() + 1, substring.end() - 1, [](char c) { return c >= '0' && c <= '9'; })) {
        try {
            InputActions::clearSource(static_cast<uintptr_t>(std::stoull(substring.substr(1, substring.size() - 2))));
        } catch (const std::exception&) {
        }
    }
}

void Engine::clearAllSubscriptions(bool includeLifecycle) {
    // Gameplay/input/update events
    onUpdate.clear();
    onFixedUpdate.clear();
    onPostUpdate.clear();
    onSceneLoaded.clear();

    onTouchStart.clear();
    onTouchEnd.clear();
    onTouchMove.clear();
    onTouchCancel.clear();

    onMouseDown.clear();
    onMouseUp.clear();
    onMouseScroll.clear();
    onMouseMove.clear();
    onMouseEnter.clear();
    onMouseLeave.clear();

    onKeyDown.clear();
    onKeyUp.clear();
    onCharInput.clear();

    onGamepadConnect.clear();
    onGamepadDisconnect.clear();
    onGamepadButtonDown.clear();
    onGamepadButtonUp.clear();
    onGamepadAxisMove.clear();
    onButtonDown.clear();
    onButtonUp.clear();

    // what scripts subscribed to the log and the console commands they added
    Log::onMessageReceived.clear();
    Device::onThermalStateChanged.clear();
    Device::onLowMemory.clear();
    DebugOverlay::removeOwnedCommands();

    if (includeLifecycle) {
        onViewLoaded.clear();
        onViewChanged.clear();
        onViewDestroyed.clear();
        onDraw.clear();
        onPause.clear();
        onResume.clear();
        onShutdown.clear();
        onFocus.clear();
        onOrientationChanged.clear();
        onVisibilityChanged.clear();
        onLowMemory.clear();
        onSafeAreaChanged.clear();
    }
}

// A script can leave callbacks on components (button.onPress.add(...)) that outlive it, and the
// code behind them goes away with the project library or the Lua state
void Engine::clearComponentSubscriptions(Scene* scene) {
    if (!scene)
        return;

    // every event of every component (ComponentEvents.h), then the physics system's: what the
    // scripts registered goes while the code behind the callbacks is still loaded
    forEachComponentEvent(*scene, [](auto& event) { event.clear(); });
    scene->getSystem<PhysicsSystem>()->clearSubscriptions();
}

void Engine::computeViewLayout(int screenWidth, int screenHeight, int preferredWidth, int preferredHeight, Scaling scalingMode,
                               int& canvasWidth, int& canvasHeight, Rect& viewRect){
    canvasWidth = preferredWidth;
    canvasHeight = preferredHeight;

    //When canvas size is changed
    if (screenWidth != 0 && screenHeight != 0){
        if (scalingMode == Scaling::FITWIDTH){
            canvasWidth = preferredWidth;
            canvasHeight = screenHeight * preferredWidth / screenWidth;
        }
        if (scalingMode == Scaling::FITHEIGHT){
            canvasHeight = preferredHeight;
            canvasWidth = screenWidth * preferredHeight / screenHeight;
        }
        if (scalingMode == Scaling::NATIVE){
            canvasHeight = screenHeight;
            canvasWidth = screenWidth;
        }
    }

    int viewX = 0;
    int viewY = 0;
    int viewWidth = screenWidth;
    int viewHeight = screenHeight;

    float screenAspect = (float)screenWidth / (float)screenHeight;
    float canvasAspect = (float)preferredWidth / (float)preferredHeight;

    //When canvas size is not changed
    if (scalingMode == Scaling::LETTERBOX){
        if (screenAspect < canvasAspect){
            float aspect = (float)screenWidth / (float)preferredWidth;
            int newHeight = (int)((float)preferredHeight * aspect);
            int dif = screenHeight - newHeight;
            viewY = (dif/2);
            viewHeight = screenHeight-(viewY*2); //diff could be odd, for this use view*2
        }else{
            float aspect = (float)screenHeight / (float)preferredHeight;
            int newWidth = (int)((float)preferredWidth * aspect);
            int dif = screenWidth - newWidth;
            viewX = (dif/2);
            viewWidth = screenWidth-(viewX*2);
        }
    }

    if (scalingMode == Scaling::CROP){
        if (screenAspect > canvasAspect){
            float aspect = (float)screenWidth / (float)preferredWidth;
            int newHeight = (int)((float)preferredHeight * aspect);
            int dif = screenHeight - newHeight;
            viewY = (dif/2);
            viewHeight = screenHeight-(viewY*2);
        }else{
            float aspect = (float)screenHeight / (float)preferredHeight;
            int newWidth = (int)((float)preferredWidth * aspect);
            int dif = screenWidth - newWidth;
            viewX = (dif/2);
            viewWidth = screenWidth-(viewX*2);
        }
    }

    // S_SCALING_STRETCH do not need nothing

    viewRect.setRect(viewX, viewY, viewWidth, viewHeight);
}

namespace {
    // refreshCanvas runs inside one of its own events (a handler that sets the canvas again)
    bool inCanvasRefresh = false;
}

void Engine::refreshCanvas(bool notify){
    const int oldWidth = Engine::canvasWidth;
    const int oldHeight = Engine::canvasHeight;
    const Rect oldViewRect = viewRect;

    computeViewLayout(System::instance().getScreenWidth(), System::instance().getScreenHeight(),
                      preferredCanvasWidth, preferredCanvasHeight, scalingMode,
                      Engine::canvasWidth, Engine::canvasHeight, viewRect);

    for (Scene* scene : getScenesSnapshot()){
        scene->updateCameraSize();
    }
    if (Scene* overlay = DebugOverlay::getScene())
        overlay->updateCameraSize();

    // the canvas mapping changed: Screen.safeArea follows (its event after onCanvasChanged)
    Screen::updateSafeArea();

    const bool changed = oldWidth != Engine::canvasWidth || oldHeight != Engine::canvasHeight ||
                         oldViewRect.getX() != viewRect.getX() || oldViewRect.getY() != viewRect.getY() ||
                         oldViewRect.getWidth() != viewRect.getWidth() || oldViewRect.getHeight() != viewRect.getHeight();
    if (!notify || !changed || inCanvasRefresh)
        return;

    inCanvasRefresh = true;
    onViewChanged.call();
    systemOrientationChanged();
    if (!paused) Screen::dispatchSafeAreaEvent();
    inCanvasRefresh = false;
}

// Project Settings > Player > Crash Handling: whether the crash handler and the hang watchdog are
// compiled in (the Exporter sets them; on otherwise). Their options are the build's PlatformConfig
// values (platform/common/PlatformDefaults.h).
#ifndef JASPERENGINE_CRASH_HANDLER
    #define JASPERENGINE_CRASH_HANDLER 1
#endif
#ifndef JASPERENGINE_CRASH_HANG_WATCHDOG
    #define JASPERENGINE_CRASH_HANG_WATCHDOG 1
#endif

namespace {
    // the game started the logger (an editor starts its own before the engine)
    bool engineOwnsLogger = false;
#ifndef JASPERENGINE_EDITOR
    // the game installed the crash handler and the watchdog (the editor does its own)
    bool engineOwnsDiagnostics = false;
#endif
    // frames systemDraw ran, stamped on log records
    uint64_t drawnFrames = 0;

    // A game's logger: the project's Player > Logging settings (the build's, PlatformConfig's
    // built*; a prebuilt player's game.json over them), then JASPER_LOG_LEVEL, then --log-level
    // on the command line
    void startGameLogger(System* system, const std::vector<std::string>& args){
        Logger::Config config;
    #if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        config.queueBytes = 256 * 1024;
    #else
        config.queueBytes = 1024 * 1024;
    #endif
        if (platformConfig().builtLogBacktrace >= 0)
            config.backtraceOnError = platformConfig().builtLogBacktrace != 0;
        if (platformConfig().logBacktrace >= 0)
            config.backtraceOnError = platformConfig().logBacktrace != 0;
    #ifndef NDEBUG
        // debug builds always do
        config.backtraceOnError = true;
    #endif
        Logger::init(config);
        engineOwnsLogger = true;
        if (!platformConfig().builtLogLevel.empty())
            Logger::applyLevelSpec(platformConfig().builtLogLevel);
        // a prebuilt player's Player > Logging level (game.json)
        if (!platformConfig().logLevel.empty()) {
            std::string error;
            if (!Logger::applyLevelSpec(platformConfig().logLevel, &error))
                fprintf(stderr, "game.json logging level: %s\n", error.c_str());
        }
        if (const char* level = std::getenv("JASPER_LOG_LEVEL")) {
            std::string error;
            if (level[0] != '\0' && !Logger::applyLevelSpec(level, &error)) {
                fprintf(stderr, "JASPER_LOG_LEVEL: %s\n", error.c_str());
            }
        }
        for (size_t i = 0; i < args.size(); i++) {
            std::string spec;
            if (args[i] == "--log-level" && i + 1 < args.size()) {
                spec = args[i + 1];
            } else if (args[i].rfind("--log-level=", 0) == 0) {
                spec = args[i].substr(12);
            } else {
                continue;
            }
            std::string error;
            if (!Logger::applyLevelSpec(spec, &error)) {
                fprintf(stderr, "--log-level: %s\n", error.c_str());
            }
        }
        system->installLogSinks();
    }

#ifndef JASPERENGINE_EDITOR
    [[maybe_unused]] const char* graphicBackendName(){
        switch (Engine::getGraphicBackend()){
            case GraphicBackend::GLCORE: return "glcore";
            case GraphicBackend::GLES3: return "gles3";
            case GraphicBackend::D3D11: return "d3d11";
            case GraphicBackend::METAL: return "metal";
            case GraphicBackend::WGPU: return "wgpu";
            case GraphicBackend::VULKAN: return "vulkan";
        }
        return "unknown";
    }

    // A game's crash reports (<user data>/crash) and, in a development build, its main-thread
    // hang watchdog (docs/crash-diagnostics.md)
    [[maybe_unused]] void startGameDiagnostics(System* system){
        // a prebuilt player's Player > Crash Handling (game.json) over the build's
        const PlatformConfig& platform = platformConfig();
        auto setting = [](int runtime, int built) { return runtime >= 0 ? runtime != 0 : built != 0; };
    #if JASPERENGINE_CRASH_HANDLER
        if (!setting(platform.crashHandler, 1)) {
            (void)system;
            return;
        }
        diag::crash::Config config;
        // the window title: the project's name (a prebuilt player's game.json, a Compile
        // build's macro, both through the platform configuration)
        // (the configuration lives for the whole run, so its text does)
        config.product = platformConfig().title.empty() ? "JasperEngine Game" : platformConfig().title.c_str();
        // the platform's path is UTF-8 (a narrow fs::path would read it in the ANSI code page)
        const std::string crashReportPath = system->getCrashReportPath();
        config.root = std::filesystem::path(std::u8string(crashReportPath.begin(), crashReportPath.end()));
        config.includeLogTail = setting(platform.crashLogTail, platform.builtCrashLogTail);
        config.dumpKind = setting(platform.crashDumpWithHeap, platform.builtCrashDumpWithHeap) ? diag::crash::DumpKind::WithHeap
                                                                                                  : diag::crash::DumpKind::Small;
        config.backend = graphicBackendName();
        config.webOverlay = setting(platform.crashWebOverlay, platform.builtCrashWebOverlay);
        #if !defined(__EMSCRIPTEN__)
        // no report root (the unit tests' System): no handler either
        if (config.root.empty()) return;
        #endif
        diag::crash::install(config);
        engineOwnsDiagnostics = true;
    #endif
    #if JASPERENGINE_DEVELOPMENT_BUILD && JASPERENGINE_CRASH_HANG_WATCHDOG && !defined(__EMSCRIPTEN__)
        // the browser owns the web's main thread
        if (!setting(platform.hangWatchdog, 1)) {
            (void)system;
            return;
        }
        diag::WatchdogConfig watch;
        watch.thresholdMs = platform.hangMs > 0 ? platform.hangMs : platform.builtHangMs;
        if (!platformConfig().title.empty())
            watch.product = platformConfig().title.c_str();
        diag::watchdog::start(watch);
        engineOwnsDiagnostics = true;
    #endif
        (void)setting;
        (void)platform;
        (void)system;
    }
#endif
}

void Engine::systemInit(int argc, char* argv[], System* system, bool startProject){
    jobs::markMainThread();

    System::setSystemInstance(system);
    // the thread that delivers async results (MainThreadQueue)
    MainThreadQueue::setMainThread();

    if (!Logger::isInitialized()) {
        startGameLogger(system, std::vector<std::string>(argv, argv + argc));
    }
    Logger::setThreadName("main");
    // the launch options' error watch and fixed frame time (player::launch parsed them)
    player::RunControl::engineStarted();
#ifndef JASPERENGINE_EDITOR
    startGameDiagnostics(system);
#endif
    // the game's timers run on this thread (Engine::systemDraw)
    Scheduler::game().setOwnerThread();

    Engine::setCanvasSize(1000,480);

    lastTime = 0;
    updateTimeCount = 0;
    viewLoaded = false;
    paused = false;
    osPaused = false;
    nativePaused = false;
    focused = true;
    visible = true;
    lastOrientation = 0;
    orientationPendingSince = 0.0;

    drawSemaphore.release();

    stm_setup();
    startupTime = stm_now();
    
    std::vector<std::string> args(argv, argv + argc);
    System::instance().args = args;

#if JASPERENGINE_PROFILER
    // the job hooks (a scope per job), and --profile / --profile-capture
    profiler::engineInit(args);
#endif

    // the build's (or a prebuilt player's game.json) VSync, then the platform's defaults until
    // init() (the generated main, a script) changes them
    vSyncCount = platformConfig().vsync ? 1 : 0;
    applyFramePacing();

    LuaBinding::createLuaState();
    // the watchdog names (and in editor Play interrupts) a runaway Lua function
    LuaWatchdog::attach();

    if (startProject)
        systemStartProject();
}

void Engine::systemStartProject(){
    #ifndef NO_LUA_INIT
    LuaBinding::init();
    // native plugins' init hooks (Apple +jasperInit) before any script can call them
    NativeBridge::initPlugins();
    #endif
    #ifndef NO_CPP_INIT
    getOnInit().call();
    #endif

    // after the game's init, which sets the project's threading settings first; anything
    // that ran jobs earlier started them with the defaults
    startJobs();
}

void Engine::setShaderCreationSettings(const ShaderCreationSettings& settings){
    SystemRender::setShaderCreationSettings(settings);
}

const ShaderCreationSettings& Engine::getShaderCreationSettings(){
    return SystemRender::getShaderCreationSettings();
}

void Engine::systemViewLoaded(){
    SystemRender::setup();
    // which graphics API and context the game really runs on (support, and the checks that a
    // gles3 player runs on an OpenGL ES context)
    Log::print("Graphics: %s; %s; %s", SystemRender::getBackendName(), SystemRender::getApiVersion().c_str(),
              SystemRender::getDeviceName().c_str());
    // the GPU's tier (spec S56 R2), logged with its reason; a player applies it from here
    Device::detect();
#ifndef JASPERENGINE_EDITOR
    // what the export recorded is created before the scenes need it (spec S32 R12)
    ShaderWarmup::beginShipped();
#endif

    if (mouseCursorType != CursorType::ARROW){
        System::instance().setMouseCursor(mouseCursorType);
    }
    if (mouseMode != MouseMode::NORMAL){
        System::instance().setMouseMode(mouseMode);
    }

    getAsyncThreadDepthStorage() = 0;

    // what init() asked for before the view existed: the swap interval needs the context,
    // the orientation and screen sleep a window
    applyFramePacing();
    Screen::applyToPlatform();
    applySimulatedSafeArea();

    // systemViewDestroyed stopped async loads; a recreated view needs them again
    SoundPool::cancelShutdown();
    TextureDataPool::cancelShutdown();

    viewLoaded = true;
    onViewLoaded.call();

    if (framebuffer){
        framebuffer->create();
    }
    
    for (Scene* scene : getScenesSnapshot()){
        scene->load();
    }
}

void Engine::systemViewChanged(){
    int screenWidth = System::instance().getScreenWidth();
    int screenHeight = System::instance().getScreenHeight();

    computeViewLayout(screenWidth, screenHeight, preferredCanvasWidth, preferredCanvasHeight, scalingMode,
                      Engine::canvasWidth, Engine::canvasHeight, viewRect);

    if (framebuffer){
        const unsigned int fbWidth = static_cast<unsigned int>(std::max(screenWidth, 0));
        const unsigned int fbHeight = static_cast<unsigned int>(std::max(screenHeight, 0));
        const bool sizeChanged = framebuffer->getWidth() != fbWidth
            || framebuffer->getHeight() != fbHeight;
        framebuffer->setWidth(fbWidth);
        framebuffer->setHeight(fbHeight);

        // Create on first use as well as on resize so the editor viewport
        // is not left at the default 512x512.
        if (fbWidth > 0 && fbHeight > 0 && (sizeChanged || !framebuffer->isCreated())){
            if (framebuffer->isCreated()){
                framebuffer->destroy();
            }
            framebuffer->create();
        }
    }

    for (Scene* scene : getScenesSnapshot()){
        scene->updateCameraSize();
    }
    if (Scene* overlay = DebugOverlay::getScene())
        overlay->updateCameraSize();

    // the canvas mapping changed: Screen.safeArea follows (its event after the two below)
    Screen::updateSafeArea();

    onViewChanged.call();

    // a resize can be a rotation: onOrientationChanged comes after onCanvasChanged
    systemOrientationChanged();

    // a backgrounded app gets it after onResume (the next frame)
    if (!paused) Screen::dispatchSafeAreaEvent();
}

// With JASPERENGINE_READY_SIGNAL set (tools/smoke-test.sh sets it), logs once the frame
// every scene drawn is fully loaded (its HTML UI documents drawn too) and no shader variant
// is still compiling, so a test
// can wait for that instead of a fixed time. A few frames in a row must be ready: a
// script creating objects in its first updates leaves the scenes loaded for a frame.
// With JASPERENGINE_FRAME_STATS=1 too, it then logs the GPU work of frames 2, 30 and 90 after
// it (SystemRender::logFrameStats): passes, draws and live resources, to
// compare builds and to see that nothing is made per frame; JASPERENGINE_FRAME_STATS=all logs
// every frame from the first (loading, and a frame that skips draws or remakes pipelines).
namespace{
    // the frames signalReadyOnce counted (frame stats: Engine::drawView names its view's line by it)
    uint64_t frameStatsFrames = 0;

    // the ready line was logged; the frames presented since (Engine::systemFramePresented)
    bool readySignalled = false;
    uint64_t presentedSinceReady = 0;
    // presented frames after the ready line before "Engine presented:" is logged: the screen
    // then shows the loaded scenes, whatever the swap chain's depth
    constexpr uint64_t PRESENTED_FRAMES_SIGNAL = 30;

    bool frameStatsEveryFrame(){
        static const bool all = [](){
            const char* value = std::getenv("JASPERENGINE_FRAME_STATS");
            return value && std::string(value) == "all";
        }();
        return all;
    }
}

bool Engine::areScenesLoaded(bool textures){
    // and no shader, pipeline or GL warm draw still being created (spec S32 R9), and no texture
    // still loading: a frame drawn meanwhile shows a stand-in for it
    bool ready = ShaderPool::getPendingShaderCount() == 0 && SystemRender::isGpuCreationIdle() && !scenes.empty() &&
                 (!textures || TextureDataPool::getLoadingCount() == 0) &&
                 // and every queued upload the frame budget left for later has run (spec S56 R8)
                 SystemRender::getDeferredUploadCount() == 0;
    // a game replaying its warm-up list holds its first scene until the list is ready (spec S32 R12)
    if (ready && SystemRender::getShaderCreationSettings().holdFirstSceneForWarmup)
        ready = ShaderWarmup::isDone();
    for (size_t i = 0; ready && i < scenes.size(); i++){
        ready = scenes[i]->getSystem<RenderSystem>()->isAllLoaded();
        // and its HTML UI documents loaded (or failed) and drawn once
        if (ready){
            const auto documents = scenes[i]->getSystem<UIDocumentSystem>();
            ready = !documents || documents->isAllLoaded();
        }
    }
    return ready;
}

std::string Engine::describeNotLoaded(){
    std::string out;
    auto add = [&out](const std::string& part){ out += (out.empty() ? "" : "; ") + part; };
    if (scenes.empty()) add("no scene");
    if (const size_t pending = ShaderPool::getPendingShaderCount()) add(std::to_string(pending) + " shader build(s) pending");
    if (const size_t uploads = SystemRender::getDeferredUploadCount())
        add(std::to_string(uploads) + " queued upload(s) waiting for the upload budget (Project Settings > Graphics > Upload Budget)");
    if (const size_t textures = TextureDataPool::getLoadingCount()){
        std::string list;
        for (const std::string& id : TextureDataPool::getLoadingIds(8)) list += (list.empty() ? "" : ", ") + id;
        add(std::to_string(textures) + " texture load(s) pending: " + list);
    }
    for (Scene* scene : scenes){
        const std::vector<std::string> waiting = scene->getSystem<RenderSystem>()->describeNotLoaded();
        if (!waiting.empty()){
            std::string list;
            for (size_t i = 0; i < waiting.size() && i < 8; i++) list += (i ? ", " : "") + waiting[i];
            if (waiting.size() > 8) list += " and " + std::to_string(waiting.size() - 8) + " more";
            add("not loaded: " + list);
        }
        const auto documents = scene->getSystem<UIDocumentSystem>();
        if (documents && !documents->isAllLoaded()) add("HTML UI documents loading or not drawn yet");
    }
    return out.empty() ? std::string("nothing (the ready line is 10 loaded frames away)") : out;
}

void Engine::signalReadyOnce(){
    static const bool enabled = [](){
        const char* value = std::getenv("JASPERENGINE_READY_SIGNAL");
        if (value && value[0] != '\0' && value[0] != '0')
            return true;
    #if defined(__ANDROID__)
        // an app gets no environment: tools/android-emulator-test.sh sets this property instead
        char property[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.jasperengine.ready_signal", property) > 0 && property[0] != '0';
    #else
        return false;
    #endif
    }();
    static const bool frameStats = [](){
        const char* value = std::getenv("JASPERENGINE_FRAME_STATS");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    // "all": every frame from the first, loading included (what a frame makes or skips)
    const bool frameStatsAll = frameStatsEveryFrame();
    static bool signalled = false;
    static int readyFrames = 0;
    uint64_t& frames = frameStatsFrames;
    static uint64_t signalledFrame = 0;
    // --after-ready counts frames from the ready line, whatever the environment says
    if (!enabled && !player::RunControl::wantsReadySignal())
        return;
    if (signalled){
        if (frameStats){
            frames++;
            const uint64_t after = frames - signalledFrame;
            // the particle simulation of this frame (every scene's systems: the CPU budget)
            size_t alive = 0;
            const double particleMs = ParticleSubsystem::takeFrameSimulation(&alive);
            if (frameStatsAll || after == 2 || after == 30 || after == 90){
                SystemRender::logFrameStats(frames);
                if (alive > 0) Log::print("Frame %llu particles_sim_ms %.3f (%zu particles)", (unsigned long long)frames, particleMs, alive);
            }
        }
        return;
    }

    frames++;
    if (frameStatsAll && frames > 1)
        SystemRender::logFrameStats(frames - 1);
    readyFrames = areScenesLoaded() ? readyFrames + 1 : 0;
    if (readyFrames >= 10){
        signalled = true;
        readySignalled = true;
        signalledFrame = frames;
        Log::print("Engine ready: every scene loaded and no shader build pending (frame %llu)", (unsigned long long)frames);
        for (Scene* scene : scenes){
            scene->getSystem<RenderSystem>()->logRenderObjects();
        }
    }
}

void Engine::systemDraw(){
    runFrame(true);
}

bool Engine::isReadySignalled(){
    return readySignalled;
}

void Engine::systemFramePresented(){
    if (!readySignalled || presentedSinceReady >= PRESENTED_FRAMES_SIGNAL)
        return;
    if (++presentedSinceReady == PRESENTED_FRAMES_SIGNAL)
        Log::print("Engine presented: %llu frames since ready", (unsigned long long)presentedSinceReady);
}

void Engine::systemUpdate(){
    // Metal frees what a frame released by frame count, and only a frame with a pass waits for
    // the GPU: frames without passes would free what the GPU still reads
    SystemRender::waitGpuIdle();
    runFrame(false);
}

void Engine::runFrame(bool draw){
    const int MAX_UPDATES_PER_FRAME = 100;

    // a game's frame starts here; the editor marks its own (FrameTimes::setExternalFrames)
    const bool ownFrame = !FrameTimes::hasExternalFrames();
    if (ownFrame){
        FrameTimes::beginFrame();
        JE_PROFILE_FRAME();
        latchShaderTime();
        // the shaders and pipelines created off the render thread that are ready, and a step of
        // the warm-up list (spec S32)
        updateGpuCreation();
    }
    JE_PROFILE_SCOPE("Frame");
    SystemRender::setViewTag(SystemRender::FRAME_STATS_FRAME);

    Logger::setFrame(++drawnFrames);
    // the game's main-loop heartbeat, and what a hang report says about the frames
    diag::heartbeat();
    diag::watchdog::setFrameInfo(drawnFrames, static_cast<float>(framePeriod * 1000.0));
#if JASPERENGINE_DEVELOPMENT_BUILD && !defined(JASPERENGINE_EDITOR)
    // JASPERENGINE_DEBUG_CRASH=<mode>@<frame> (web: ?jasperengine_debug_crash=<mode>)
    // without a frame: 30 frames after every scene loaded (after the ready signal's line)
    static const std::optional<diag::crash::DebugCrashRequest> debugCrashRequest = diag::crash::debugCrashRequest();
    static uint64_t debugCrashLoadedFrames = 0;
    if (debugCrashRequest){
        debugCrashLoadedFrames = areScenesLoaded() ? debugCrashLoadedFrames + 1 : 0;
        const bool due = debugCrashRequest->frame != 0 ? drawnFrames == debugCrashRequest->frame : debugCrashLoadedFrames == 30;
        if (due) diag::crash::debugCrash(debugCrashRequest->mode);
    }
#endif
#ifndef JASPERENGINE_EDITOR
    for (const diag::HangNotice& notice : diag::watchdog::takeNotices()){
        Log::warn("The game was not responding for %.1f s (%s). Details saved%s%s", notice.stalledMs / 1000.0,
                  notice.label.c_str(), notice.file.empty() ? "" : ": ", notice.file.string().c_str());
    }
#endif

    // a quarter turn waiting for its view size (systemOrientationChanged's timeout)
    if (orientationPendingSince > 0.0){
        systemOrientationChanged();
    }
    // insets the platform pushed between frames (held while the app is paused)
    if (!paused) Screen::dispatchSafeAreaEvent();

    //Deltatime in seconds
    double rawDelta = stm_sec(stm_laptime(&lastTime));
    const uint64_t frameStart = lastTime;
    uint64_t drawStart = frameStart;
    framerate = (rawDelta > 0.0) ? (float)(1.0 / rawDelta) : 0.0f;
    framePeriod = rawDelta;
    // (the stats overlay's frame times are S15's frame-time ring, FrameTimes, from systemDraw)

    // Cap the delta exposed to gameplay (Engine::getDeltatime) and the update loop.
    // Anything bigger than maxDeltatime gets clamped; the simulation effectively
    // skips the lost time instead of teleporting objects on a stalled frame.
    double frameTime = (rawDelta > maxDeltatime) ? maxDeltatime : rawDelta;
    // the first frame after a hold measured the hold
    if (clampNextDelta && !simulationHeld){
        clampNextDelta = false;
        if (updateTime > 0.0 && frameTime > updateTime) frameTime = updateTime;
    }
    // a fixed frame delta replaces the measured one; the time scale applies to both
    if (fixedDeltatime > 0.0){
        frameTime = fixedDeltatime;
    }
    deltatime = simulationHeld ? 0.0 : frameTime * timeScale;
    // the unscaled delta (Time.unscaledDeltaTime, realtime waits): it runs at time scale 0 (a
    // pause menu animates) and stops with the game: a held simulation, a paused app, or the game
    // events paused (the editor's Play pause, edit time)
    // (getUnscaledDeltatime(): also 0 while Engine::pauseGameEvents(true), the editor's Play pause)
    unscaledDeltatime = (paused || simulationHeld || !onUpdate.isEnabled()) ? 0.0 : frameTime;
    // the UI clock: the same, but it keeps running while only the game events are paused (HTML
    // UI documents animate in edit mode and in a paused Play)
    HtmlUIClock::advance((paused || simulationHeld) ? 0.0 : frameTime);
    // the audio mixer's snapshot transitions and timed values, once a frame before the scenes
    // (scaled ones hold at time scale 0; both hold while the app or the simulation is held, and on
    // the deterministic driver while its pump holds: what a Play's pause lasted is not in the mix)
    const bool audioHeld = paused || simulationHeld || (!onUpdate.isEnabled() && AudioSystem::getOutputMode() == AudioOutputMode::Deterministic);
    AudioMixer::update(audioHeld ? 0.0 : deltatime, audioHeld ? 0.0 : frameTime);

    // timers: what other threads posted, then the real clock (runs paused or loading too)
    Scheduler& scheduler = Scheduler::game();
    scheduler.processPosted();
    scheduler.advance(TimeDomain::Real, rawDelta);
    scheduler.process(TimeDomain::Real);

    {
        JE_PROFILE_SCOPE("Input");
        // the input of this frame is over: no low surrogate will follow a pending high one
        flushCharInput();
        // held gamepad directions repeat on the UI clock
        updateGamepadNavigation();
    }

    drawSemaphore.acquire();

    // lines other threads logged since the last frame, to the main-thread log listeners
    // (Log.onMessageReceived), first thing of the frame
    Log::dispatchPending();

    // native SDKs' engine pause requests and native call timeouts (paused frames too)
    NativeBridge::update();

    // the platform's thermal state and memory warnings since the last frame (paused frames too:
    // a hot device recovers while the game waits), then what a new tier or step changed
    Device::update(rawDelta);
    applyQualityChanges();

    // memory the last-but-one frame allocated per frame goes back now
    memory::frameArena().beginFrame();
    // job results posted for the main thread, then (single-thread mode) the queued jobs:
    // everything urgent, Background work for the pump budget
    jobs::drainMainQueue();
    if (jobs::hasSystem()){
        jobs::JobSystem& jobSystem = jobs::system();
        if (jobSystem.isSingleThreaded())
            jobSystem.pump(threadingSettingsStorage().webPumpBudgetMs);
        jobSystem.sampleFrame();
    }

    {
        JE_PROFILE_SCOPE("SokolCmdQueue execute");
        // async uploads of the last frame, while the scenes that queued them still exist
        SystemRender::executeQueue();
    }

    {
        JE_PROFILE_SCOPE("Texture loads");
        // progressive mip swaps, main-thread transcodes, budget reloads (cooked textures)
        TextureDataPool::update();
    }

    {
        JE_PROFILE_SCOPE("Shader compile");
        // the main-thread share of runtime shader compilation (one variant a frame when
        // compiling without a worker thread)
        RuntimeShaderCompiler::beginFrame();
    }

    frameRunning = true;

    // what DontDestroyOnLoad / MoveGameObjectToScene asked for between frames (input
    // callbacks, the editor's tools), at this frame's first safe point
    SceneManager::flushPersistentMoves();

#if JASPERENGINE_AUDIO_CAPTURE && !defined(JASPERENGINE_EDITOR)
    // --audio-capture: from the first frame of the start scene, before its update
    player::RunControl::beginAudioCapture();
#endif

    // a transition requested during the last frame; one requested by its factory waits again
    SceneManager::applyPendingLoad();

    // a --screenshot run's capture frame draws offscreen (R20); a run without it never does
    captureFrame = draw && player::RunControl::isActive() && player::RunControl::wantsOffscreenFrame();

    // before the update, which bakes the PIP_RTT pipelines from the target in use
    if (draw)
        beginCompositeFramebuffer();

    // avoid increment updateTimeCount after resume; a held simulation only draws
    if (!paused && !simulationHeld) {
        // the input of the frame: the events since the last running frame (Input.h), then
        // the actions it drives (their callbacks, onButtonDown / onButtonUp run here, before the
        // fixed steps), on the unscaled frame time
        Input::beginFrame(frameTime);

        double frameDelta = deltatime;
        gameTime += frameDelta;
        gameFrames++;
        // the scenes' game-time clocks (Scene::getSceneTime), before the fixed steps, so the
        // physics, the update and the draw of the frame see one value; a paused scene holds
        for (int i = 0; i < scenes.size(); i++)
            scenes[i]->advanceSceneTime(frameDelta);
        // the scheduler's game clocks: stopped by the editor's Pause too (gameTime is not)
        const bool gameClocks = advanceGameClocks(scheduler, frameTime);

        unscaledTime += unscaledDeltatime;
        smoothDeltatime = (gameFrames <= 1) ? (float)frameDelta : smoothDeltatime + 0.2f * ((float)frameDelta - smoothDeltatime);

        // the script coroutines and timers advance with the game events: not at edit time nor
        // while the editor's Play is paused (Engine::pauseGameEvents)
        if (onUpdate.isEnabled())
            ScriptScheduler::beginFrame(frameDelta, unscaledDeltatime);

        // transform-mode Camera Shakers write their base back before anything of this frame
        // moves their GameObjects (they shake it again in the scene update)
        for (int i = 0; i < scenes.size(); i++) {
            if (auto cameras = scenes[i]->findSystem<CameraControlSystem>()) cameras->preUpdate();
        }

        // 0) start of the scripts enabled since the last frame, before their first fixed
        //    update and update (Unity)
        ScriptRuntime::flushStarts();

        // 1) Fixed-timestep phase (deterministic). Runs zero or more times per frame.
        //    Used for physics, networking, and any subsystem overriding fixedUpdate().
        if (updateTime > 0.0) {
            JE_PROFILE_SCOPE("FixedUpdate");
            updateTimeCount += frameDelta;

            int fixedLoops = 0;
            while (updateTimeCount >= updateTime && fixedLoops < MAX_UPDATES_PER_FRAME) {
                // edges read in the step are the fixed window's: each is seen by one step
                Input::beginFixedStep();
                // counted like this loop, so its last step sees 1
                fixedStepsLeft = 0;
                for (double remaining = updateTimeCount; remaining >= updateTime && fixedLoops + fixedStepsLeft < MAX_UPDATES_PER_FRAME; remaining -= updateTime) {
                    fixedStepsLeft++;
                }

                inFixedStep = true;
                ScriptScheduler::beginFixedStep();
                Engine::onFixedUpdate.call();
                for (int i = 0; i < scenes.size(); i++) {
                    scenes[i]->fixedUpdate(updateTime);
                }
                if (gameClocks) {
                    scheduler.advance(TimeDomain::FixedSteps, 1.0);
                    scheduler.process(TimeDomain::FixedSteps);
                }
                Input::endFixedStep();
                InputActions::endFixedStep();
                // after the physics step: Coroutine.waitForFixedUpdate
                ScriptScheduler::fixedPhase();
                fixedTime += updateTime;
                inFixedStep = false;
                updateTimeCount -= updateTime;
                fixedLoops++;
            }

            fixedStepsLeft = 1;

            if (fixedLoops >= MAX_UPDATES_PER_FRAME) {
                Log::warn("Dropping fixed-update steps - more than %i per frame", MAX_UPDATES_PER_FRAME);
                // Discard remaining backlog so we don't fall further behind next frame.
                updateTimeCount = 0.0;
            }

            // bodies drawn between their last two steps, as far as the time left over
            double alpha = getInterpolationAlpha();
            for (int i = 0; i < scenes.size(); i++) {
                scenes[i]->getSystem<PhysicsSystem>()->interpolate(alpha);
            }
        }

        // 2) Variable-timestep phase. Runs exactly once per frame with the real frame delta.
        //    Used for animation, input, UI, and any subsystem overriding update().
        JE_PROFILE_SCOPE("Update");
        // What other threads finished (HTTP responses, native callbacks) reaches the game here,
        // right before onUpdate, while game events run (not in the editor outside Play)
        if (Engine::onUpdate.isEnabled()) {
            MainThreadQueue::drain();
        }
        Engine::onUpdate.call();
        for (int i = 0; i < scenes.size(); i++) {
            scenes[i]->update(frameDelta);
        }
        // timers (the Scheduler's, the Lua Timer's among them), then the coroutines waiting
        // seconds, frames, predicates, timers or other work: after the update, before the
        // post-update (Unity's yield null / WaitForSeconds point: after Update, before
        // LateUpdate). A coroutine waiting for a timer resumes in the frame the timer ends.
        if (gameClocks) {
            scheduler.process(TimeDomain::Game);
            scheduler.process(TimeDomain::Unscaled);
            scheduler.process(TimeDomain::Frames);
        }
        ScriptScheduler::updatePhase();
        Engine::onPostUpdate.call();

        // GameObject::destroy: after the update, before the draw (Unity)
        ObjectManager::update(frameDelta);
        // their holes in the component arrays closed now, not by whatever reads next
        for (int i = 0; i < scenes.size(); i++) {
            scenes[i]->compactComponents();
        }
        // what was for this frame only (the virtual sources' deltas) goes
        Input::endFrame();
    }

#if JASPERENGINE_SOUND_LAYER
    // the music's callbacks and segments for what this frame mixes (docs/specs/S39-music.md)
    MusicPlayer::update(unscaledDeltatime);
#endif
    // after the scenes, so a voice, what started it and the frame's mix are on the same sample
    AudioSystem::pumpDeterministic(unscaledDeltatime);

    // the debug overlay updates on its own clock: also while the time scale is 0 or the
    // editor holds the game, so it stays usable on a frozen game
    if (DebugOverlay::needsUpdate())
        DebugOverlay::update(paused ? 0.0 : frameTime);

    // what is still being created off the render thread (spec S32) does not draw this frame
    const bool gpuCreationIdleForDraw = SystemRender::isGpuCreationIdle();

    if (draw){
        Engine::onDraw.call();

        // again for a scene stack loaded during the update
        beginCompositeFramebuffer();

        beginFrameStats();
        drawStart = stm_now();
        {
            JE_PROFILE_SCOPE("Draw");
            for (int i = 0; i < scenes.size(); i++){
                scenes[i]->draw();
            }

            // the debug overlay above every scene (the loading scene too), outside the game's
            // frame statistics
            if (DebugOverlay::getScene()){
                const FrameStats gameFrameStats = currentFrameStats;
                DebugOverlay::draw();
                currentFrameStats = gameFrameStats;
            }
            endCompositeFramebuffer();
        }
    }

    {
        JE_PROFILE_SCOPE("Commit");
        SystemRender::commit();
    }
    frameCpuTime = stm_sec(stm_since(frameStart));
    frameDrawCpuTime = draw ? stm_sec(stm_since(drawStart)) : 0.0;
    FrameTimes::setDrawCalls(currentFrameStats.drawCalls);

    // a one-shot stays until a frame draws it: loaded means its objects are made or asked for, and
    // one whose program or pipeline was still being made draws nothing (a thumbnail, blank on Vulkan)
    if (draw && gpuCreationIdleForDraw){
        std::lock_guard<std::mutex> lock(sceneListMutex);

        // Retire one-shots that finished loading: they got the frame they asked for.
        for (auto it = oneTimeScenes.begin(); it != oneTimeScenes.end(); ) {
            Scene* scene = *it;

            if (!scene->getSystem<RenderSystem>()->isAllLoaded()) {
                ++it;
                continue;
            }

            // The main scene draws every frame regardless, so only drop the registration.
            if (scene != mainScene) {
                auto sceneIt = std::find(scenes.begin(), scenes.end(), scene);
                if (sceneIt != scenes.end()) {
                    scenes.erase(sceneIt);
                }
            }

            it = oneTimeScenes.erase(it);
        }
    }

    // Coroutine.waitForEndOfFrame: after the draw (nothing when the update did not run)
    ScriptScheduler::endOfFramePhase();

    // after the draw, so the new scenes are drawn loaded before the loading scene goes
    SceneManager::updateLoading();

    // it counts drawn frames
    if (draw)
        signalReadyOnce();

    // the launch options' frame count, screenshot and quit (docs/specs/S30-shipping.md R17)
    if (draw && player::RunControl::isActive()){
        const bool captured = captureFrame;
        captureFrame = false;
        player::RunControl::frameDrawn(readySignalled, captured ? player::FrameReader(&Engine::readCompositeFrame) : player::FrameReader());
    }

#if JASPERENGINE_PROFILER
    // the built-in monitors, and the command line's capture once the scenes are loaded
    profiler::engineFrameEnd(areScenesLoaded());
#endif

    // desktop UserSettings: written a second after a change, not on every set
    UserSettings::tick();

    if (ModelCpuData::getPendingCount() > 0)
        ModelCpuData::update(getScenesSnapshot());

    frameRunning = false;

    if (ownFrame)
        FrameTimes::endFrameWork();

    drawSemaphore.release();
}

#ifdef JASPERENGINE_EDITOR
void Engine::drawView(uint32_t viewId, Framebuffer* target, const std::vector<ViewLayer>& layers){
    if (!viewLoaded || !target || layers.empty())
        return;

    drawSemaphore.acquire();

    JE_PROFILE_SCOPE("Draw View");
    // the frame statistics are the game's: a second view of it adds nothing to them
    const FrameStats gameFrameStats = currentFrameStats;
    // its passes and GPU time are the view's
    SystemRender::setViewTag(viewId);

    bool baseLayer = true;
    for (const ViewLayer& layer : layers){
        if (!layer.scene)
            continue;

        RenderSystem::ViewDesc desc;
        desc.camera = layer.camera;
        desc.target = target;
        desc.viewRect = layer.viewRect;
        desc.baseLayer = baseLayer;
        desc.editorOverlays = layer.editorOverlays;
        desc.fixedResolution = layer.fixedResolution;
        desc.disableFog = layer.disableFog;
        desc.disableFaceCulling = layer.disableFaceCulling;
        desc.postProcessing = layer.postProcessing;
        desc.disableClouds = layer.disableClouds;
        desc.atmosphereQuality = layer.atmosphereQuality;
        desc.shadows = layer.shadows;
        desc.drawMode = (ViewDrawMode)layer.drawMode;
        desc.canvasWidth = layer.canvasWidth;
        desc.canvasHeight = layer.canvasHeight;
        layer.scene->getSystem<RenderSystem>()->drawView(viewId, desc);

        baseLayer = false;
    }

    currentFrameStats = gameFrameStats;

    SystemRender::commit();
    SystemRender::setViewTag(SystemRender::FRAME_STATS_FRAME);
    // JASPERENGINE_FRAME_STATS=all: this view's passes too (each view commits on its own)
    if (frameStatsEveryFrame())
        SystemRender::logFrameStats(frameStatsFrames, viewId);

    drawSemaphore.release();
}
#endif

void Engine::systemViewDestroyed(){
    // a launch that failed before systemViewLoaded (macOS without Metal), or a second destroy:
    // there is nothing to tear down, and sokol was never set up
    if (!viewLoaded)
        return;

    SoundPool::requestShutdown();
    TextureDataPool::requestShutdown();

    drawSemaphore.acquire();
    
    viewLoaded = false;
    Engine::onViewDestroyed.call();

    // the overlay's Scene goes with the GPU objects (it is made again when shown)
    DebugOverlay::viewDestroyed();

    //TODO: must destroy all scenes (even if not a layer)
    for (int i = 0; i < scenes.size(); i++){
        scenes[i]->destroy();
    }

    {
        std::lock_guard<std::mutex> lock(sceneListMutex);
        oneTimeScenes.clear();
    }

    SystemRender::shutdown();

    compositeFramebuffer.destroy();

    clearPools();

    drawSemaphore.release();
}

void Engine::systemShutdown(){
#ifndef JASPERENGINE_EDITOR
    // teardown is watched on its own window; the job system is not read past here
    if (engineOwnsDiagnostics) diag::watchdog::beginShutdownWatch();
#endif
    // Unity's quit: onApplicationQuit to every script, then onDisable and onDestroy (the OS
    // may end a mobile app without this)
    ScriptRuntime::quitAll();

    Engine::onShutdown.call();

#if JASPERENGINE_AUDIO_CAPTURE
    // a player's capture ends with the run; its voices are still in their scenes
    AudioCapture::end();
#endif

    // the overlay (and its Lua commands) goes while the Lua state still exists
    DebugOverlay::shutdown();

    // what the game (and its onShutdown handlers) stored is on disk before it goes
    UserSettings::save();

    // anime's, the timers', the script coroutines' and the input actions' Lua references go
    // while the Lua state still exists
#if JASPERENGINE_HTMLUI
    AnimEngine::instance().reset();
#endif
    Scheduler::game().clear();
    ScriptScheduler::clear();
    InputActions::unload();
    // the persistent scene's scripts (their onDestroy ran above) are released, and it goes
    SceneManager::destroyPersistentScene();

    // HTTP requests end without their callbacks and the HTTP stack's threads stop; what other
    // threads posted for the game is dropped
    Http::terminate();
    // the media cache's downloads are forgotten (their .part files stay for a resume)
    MediaCache::shutdown();
    // native calls end without their callbacks, the event handlers go
    NativeBridge::shutdown();
    MainThreadQueue::clear();
    LuaBinding::cleanup();

    removeAllSceneLayers(true);

    // the readers submit decode jobs, so they stop first; queued submit() jobs end
    // Cancelled, enqueueDetached() jobs run, every thread joins
    AssetIOService::instance().stop();
#if JASPERENGINE_PROFILER
    profiler::engineShutdown();
#endif
    jobs::shutdown();

#ifndef JASPERENGINE_EDITOR
    // a clean exit: no session marker is left for the next start to find
    if (engineOwnsDiagnostics) {
        engineOwnsDiagnostics = false;
        diag::watchdog::stop();
        diag::crash::shutdown();
    }
#endif

    // and what scripts stored while their scenes went (onDestroy): the last write. Desktop
    // System objects live until the process ends, so ~System never gets to write it.
    UserSettings::save();

    // what the game logged (the workers' last lines too) reaches its sinks before the process goes
    if (engineOwnsLogger) {
        engineOwnsLogger = false;
        Logger::shutdown();
    }
}

void Engine::systemPause(){
    if (osPaused) return;
    // leaving: the focus goes first (Unity: OnApplicationFocus(false), then OnApplicationPause(true))
    systemFocus(false);
    osPaused = true;
    applyPause();
}

void Engine::systemResume(){
    if (!osPaused) return;
    osPaused = false;
    applyPause();
}

void Engine::setNativePause(bool held){
    nativePaused = held;
    applyPause();
}

void Engine::applyPause(){
    const bool want = osPaused || nativePaused;
    if (want == paused) return;
    paused = want;
    // a backgrounded app expects no heartbeat; resume re-arms on the next frame
    diag::watchdog::setPaused(want);
    if (!want){
        AudioSystem::setPlatformPaused(false);
        ScriptRuntime::broadcast(ScriptMessage::ApplicationPause, [](lua_State* L) { lua_pushboolean(L, 0); return 1; },
            [](ScriptBase& script) { script.onApplicationPause(false); });
        Engine::onResume.call();
        return;
    }
    AudioSystem::setPlatformPaused(true);
    ScriptRuntime::broadcast(ScriptMessage::ApplicationPause, [](lua_State* L) { lua_pushboolean(L, 1); return 1; },
        [](ScriptBase& script) { script.onApplicationPause(true); });
    Engine::onPause.call();

    // the key and button ups of a paused app never come: what is held comes up now (S22's
    // focus release; systemFocus(false) or the platform did it already unless the window kept
    // the focus)
    if (windowFocused)
        systemFocusChanged(false);
    // so the presses of the documents and the component UI end now (cancelled: no click), the
    // controls let go (D6)
    cancelAllUIPointers();

    // a paused app may be killed without another call (after the handlers, which may set some)
    UserSettings::save();
}

void Engine::systemFocus(bool hasFocus){
    if (hasFocus == focused) return;
    // coming back: the resume goes first (OnApplicationPause(false), then OnApplicationFocus(true))
    if (hasFocus && osPaused){
        systemResume();
    }
    focused = hasFocus;
    // the key and button ups after a focus loss go to another window: the next frame sees them; the
    // UI presses end now (D6, cancelled)
    if (!hasFocus){
        // held input comes up through S22's focus release (the platforms call
        // systemFocusChanged first: then nothing is held any more)
        if (windowFocused)
            systemFocusChanged(false);
        cancelAllUIPointers();
    }
    ScriptRuntime::broadcast(ScriptMessage::ApplicationFocus, [hasFocus](lua_State* L) { lua_pushboolean(L, hasFocus ? 1 : 0); return 1; },
        [hasFocus](ScriptBase& script) { script.onApplicationFocus(hasFocus); });
    Engine::onFocus.call(hasFocus);
}

void Engine::systemVisibilityChanged(bool isVisible){
    if (isVisible == visible) return;
    visible = isVisible;
    if (isVisible)
        Log::print("Window visible: drawing again");
    else
        Log::print("Window hidden: updating at %g Hz without drawing", getHiddenUpdateFps());
    Engine::onVisibilityChanged.call(isVisible);
}

void Engine::systemOrientationChanged(){
    if (!System::hasInstance()) return;
    const int orientation = (int)Screen::getOrientation();
    if (orientation == lastOrientation){
        orientationPendingSince = 0.0;
        // safe-area insets that came while a quarter turn waited (it was undone)
        Screen::releaseHeldSafeArea();
        return;
    }
    // the first one is where the view starts, not a change
    if (lastOrientation == 0){
        lastOrientation = orientation;
        return;
    }
    // A quarter turn is reported before the new view size on some platforms (Android's display
    // rotation, the web's screen.orientation): it waits for the size to flip, so onCanvasChanged
    // comes first (systemViewChanged calls this again), or ORIENTATION_SIZE_TIMEOUT seconds (a view
    // that keeps its shape, e.g. a fixed canvas in a page). A half turn changes no size: at once.
    auto landscape = [](int o){
        return o == (int)ScreenOrientation::LandscapeLeft || o == (int)ScreenOrientation::LandscapeRight;
    };
    if (landscape(orientation) != landscape(lastOrientation)){
        const int width = System::instance().getScreenWidth();
        const int height = System::instance().getScreenHeight();
        const bool sizeFlipped = width == height || landscape(orientation) == (width > height);
        const double now = getSystemTime();
        if (!sizeFlipped){
            if (orientationPendingSince <= 0.0) orientationPendingSince = now;
            if (now - orientationPendingSince < ORIENTATION_SIZE_TIMEOUT) return;
        }
    }
    orientationPendingSince = 0.0;
    lastOrientation = orientation;
    Engine::onOrientationChanged.call(orientation);
    // the insets the platform pushed while the turn waited for its size (the timeout: a view that
    // keeps its shape); after a size change systemViewChanged already used them
    Screen::releaseHeldSafeArea();
}

bool Engine::isOrientationPending(){
    return orientationPendingSince > 0.0;
}

void Engine::systemLowMemory(){
    Engine::onLowMemory.call();
    // the response (caches, texture mips, Device.onLowMemory) runs at the next frame's start
    Device::reportLowMemory();
}

void Engine::applySimulatedSafeArea(){
    if (!System::hasInstance()) return;
    const Platform platform = getPlatform();
    if (platform != Platform::Linux && platform != Platform::Windows && platform != Platform::MacOS) return;
    // the editor simulates devices in its Game view
    if (System::instance().isEditor()) return;
    EdgeInsets insets;
    if (System::instance().getSimulatedSafeArea(insets)){
        Screen::setPlatformSafeArea(insets);
        Log::print("Simulated safe area (JASPERENGINE_SIMULATE_SAFE_AREA): top %g, right %g, bottom %g, left %g pixels",
                  insets.top, insets.right, insets.bottom, insets.left);
    }
}

void Engine::resetApplicationState(){
    osPaused = false;
    nativePaused = false;
    if (paused){
        paused = false;
        AudioSystem::setPlatformPaused(false);
    }
    focused = true;
    visible = true;
    lastOrientation = 0;
    orientationPendingSince = 0.0;
    targetFrameRate = -1;
    // no tier a script set, no Game view simulation, the thermal and memory response at rest
    Device::resetSession();
    applyFramePacing();
    dynamicBatching = dynamicBatchingDefault;
    // no extra insets, and the next safe area is where the game starts (no event)
    Screen::resetSafeAreaState();
}

bool Engine::isFocused(){
    return focused;
}

bool Engine::isPaused(){
    return paused;
}

void Engine::setDynamicBatchingDefault(bool enabled){
    dynamicBatchingDefault = enabled;
    dynamicBatching = enabled;
}

void Engine::setDynamicBatching(bool enabled){
    dynamicBatching = enabled;
}

bool Engine::isDynamicBatching(){
    return dynamicBatching;
}

bool Engine::isVisible(){
    return visible;
}

double Engine::getHiddenUpdateFps(){
    const double refreshRate = System::hasInstance() ? System::instance().getRefreshRate() : 0.0;
    return PlatformRules::hiddenUpdateFps(targetFrameRate, vSyncCount, refreshRate);
}

void Engine::setTargetFrameRate(int fps){
    targetFrameRate = fps <= 0 ? -1 : std::min(fps, 1000);
    applyFramePacing();
}

int Engine::getTargetFrameRate(){
    return targetFrameRate;
}

void Engine::setVSyncCount(int count){
    vSyncCount = std::clamp(count, 0, 4);
    applyFramePacing();
}

int Engine::getVSyncCount(){
    return vSyncCount;
}

const FramePacing& Engine::getFramePacing(){
    return framePacing;
}

void Engine::applyQualityChanges(){
    const uint32_t revision = QualityResolver::getRevision();
    if (revision == qualityRevision)
        return;
    qualityRevision = revision;
    applyFramePacing();
    TextureDataPool::setTextureQuality(QualityResolver::textureQuality(textureQuality));
    SystemRender::setUploadBudget(QualityResolver::uploadBudgetBytes());
}

void Engine::applyFramePacing(){
    if (!System::hasInstance()) return;
    System& system = System::instance();
    // the device tier's and the thermal response's cap on the game's rate (QualityResolver)
    framePacing = PlatformRules::computeFramePacing(getPlatform(), QualityResolver::targetFrameRate(targetFrameRate), vSyncCount,
                                                    system.getRefreshRate(), system.getMaxSwapInterval(),
                                                    system.canDisableVSync());
    system.applyFramePacing(framePacing);
}

// Scenes below a loading scene only get pointer-up, so nothing stays pressed
static bool canSendUIEvents(Scene* scene){
    return scene->canReceiveUIEvents() && (scene == DebugOverlay::getScene() || !SceneManager::isCoveredByLoading(scene));
}

// documents are an explicit request for input: they receive it unless the scene turns
// UI events off (UIEventState::DISABLED) or a loading scene covers it (never the debug
// overlay's, which is above the loading scene)
static bool canSendDocumentEvents(Scene* scene){
    return scene->getEnableUIEvents() != UIEventState::DISABLED && (scene == DebugOverlay::getScene() || !SceneManager::isCoveredByLoading(scene));
}

std::vector<Scene*> Engine::getInputScenes(bool keys){
    std::vector<Scene*> inputScenes = getScenesSnapshot();
    // the debug overlay is the top layer: its document is offered input first and consumes
    // it over its own elements only; keys and text only while its search or command field
    // has the focus
    if (Scene* overlay = DebugOverlay::getScene()){
        if (!keys || DebugOverlay::hasTextFocus())
            inputScenes.push_back(overlay);
    }
    return inputScenes;
}

bool Engine::transformCoordPos(float& x, float& y){

    x = (x - viewRect.getX()) / viewRect.getWidth();
    y = (y - viewRect.getY()) / viewRect.getHeight();
    
    x = (float)Engine::getCanvasWidth() * x;
    y = (float)Engine::getCanvasHeight() * y;
    
    if (allowEventsOutCanvas || mouseMode == MouseMode::CAPTURED)
        return true;
    
    return ((x >= 0) && (x <= Engine::getCanvasWidth()) && (y >= 0) && (y <= Engine::getCanvasHeight()));
}

// ---- input. Order per pointer event (phase-1 spec §3.6.1, §3.15): HTML UI screen documents,
// top scene layer first (the first scene whose document consumes stops the document walk);
// then the component UI of every scene, except that a scene whose document took a press does
// not give it to its component UI; then the documents on meshes (texture mode), which the
// screen-space UI covers; then the game, unless the UI consumed it and ignoreEventsHandledByUI
// is set. Keys and text: documents, then the game, then the component UI.

namespace {

    // character input one unit at a time (systemCharInput) to code points
    Utf16Combiner charInputCombiner;

    // a document set the cursor on the last pointer move (systemMouseMove)
    bool documentCursorSet = false;

    std::string toUtf8(const std::vector<uint32_t>& codepoints){
        std::string out;
        for (uint32_t codepoint : codepoints){
            if (codepoint < 0x80){
                out += (char)codepoint;
            }else if (codepoint < 0x800){
                out += (char)(0xC0 | (codepoint >> 6));
                out += (char)(0x80 | (codepoint & 0x3F));
            }else if (codepoint < 0x10000){
                out += (char)(0xE0 | (codepoint >> 12));
                out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                out += (char)(0x80 | (codepoint & 0x3F));
            }else{
                out += (char)(0xF0 | (codepoint >> 18));
                out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                out += (char)(0x80 | (codepoint & 0x3F));
            }
        }
        return out;
    }

    // the documents of the scenes that take document events, top layer first
    std::vector<UIDocumentSystem*> documentSystemsTopFirst(const std::vector<Scene*>& scenes){
        std::vector<UIDocumentSystem*> systems;
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if (canSendDocumentEvents(*it))
                systems.push_back((*it)->getSystem<UIDocumentSystem>().get());
        }
        return systems;
    }

    // offers an event to the documents, top layer first; the scene whose document consumed it, or null
    Scene* dispatchToDocuments(const std::vector<Scene*>& scenes, const std::function<bool(UIDocumentSystem*)>& event){
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if (!canSendDocumentEvents(*it))
                continue;
            if (event((*it)->getSystem<UIDocumentSystem>().get()))
                return *it;
        }
        return nullptr;
    }

    // A release reaches the documents of every scene not turned off, even one a loading scene
    // covers, so nothing stays pressed. Above the scene that consumes it, documents get it only
    // when they hold the pointer.
    bool dispatchReleaseToDocuments(const std::vector<Scene*>& scenes, int pointer, int button, bool touch,
                                    const std::function<bool(UIDocumentSystem*)>& event){
        bool consumed = false;
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if ((*it)->getEnableUIEvents() == UIEventState::DISABLED)
                continue;
            UIDocumentSystem* documents = (*it)->getSystem<UIDocumentSystem>().get();
            if (consumed && !documents->hasPointerCapture(pointer, button, touch))
                continue;
            if (event(documents))
                consumed = true;
        }
        return consumed;
    }

    // Documents on meshes (texture mode) are part of the 3D world, which the screen documents
    // and the component UI are drawn over: they are offered a pointer event after both, top
    // layer first. covered: screen-space UI took the event (or a world document of a scene
    // above did), so they only lose the pointer (hover, key focus). A pointer a world document
    // holds since its press went to it with the screen documents (UIDocumentSystem::
    // eventOnPointer*, capture). release: the looser gate of releases (a covered scene too).
    bool dispatchToWorldDocuments(const std::vector<Scene*>& scenes, bool covered, bool release,
                                  const std::function<bool(UIDocumentSystem*, bool)>& event){
        bool consumed = false;
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if (release ? (*it)->getEnableUIEvents() == UIEventState::DISABLED : !canSendDocumentEvents(*it))
                continue;
            if (event((*it)->getSystem<UIDocumentSystem>().get(), covered || consumed))
                consumed = true;
        }
        return consumed;
    }

    // the game's and the component UI's character input: one whole code point per call
    void sendCharacterToGame(uint32_t codepoint, bool toComponentUI, bool toGame, const std::vector<Scene*>& scenes){
        if (toGame)
            Engine::onCharInput.call(static_cast<char32_t>(codepoint));
        if (toComponentUI){
            for (Scene* scene : scenes){
                if (canSendUIEvents(scene))
                    scene->getSystem<UISystem>()->eventOnCharInput(static_cast<char32_t>(codepoint));
            }
        }
    }

    // ---- gamepads to documents (phase 4, p4-input): buttons, the left stick's directions and the
    // triggers become keys of the focused document (UIDocumentSystem::eventOnGamepadKeyDown/Up)

    constexpr double gamepadRepeatDelay = 0.4;       // seconds a direction is held before it repeats
    constexpr double gamepadRepeatInterval = 0.1;    // then one repeat per interval
    constexpr float stickPressThreshold = 0.5f;      // a left stick direction is pressed past this
    constexpr float stickReleaseThreshold = 0.3f;    // and released below it
    constexpr float triggerPressThreshold = 0.0f;    // triggers rest at -1: pressed past half way
    constexpr float triggerReleaseThreshold = -0.2f;

    // who took a held gamepad key: its ups and repeats go there (ui-components spec 6.5)
    enum class GamepadUIOwner : uint8_t { NONE, DOCUMENTS, COMPONENT };

    struct GamepadUIHeld {
        int gamepad;
        GamepadUIKey key;
        GamepadUIOwner owner;   // a document or the component UI's selection took the down
        double nextRepeat;      // UI clock
    };

    struct GamepadUIState {
        std::vector<GamepadUIHeld> held;
        std::map<int, GamepadUIKey> stick;                 // the direction each pad's left stick holds
        std::map<std::pair<int, int>, float> gameAxes;     // the last value of each axis the game got
        std::set<std::pair<int, int>> takenAxes;           // the axes the UI holds from the game now
    };
    GamepadUIState gamepadUI;

    GamepadUIKey gamepadKeyOfButton(int button){
        switch (button){
            case D_GAMEPAD_BUTTON_A: return GamepadUIKey::A;
            case D_GAMEPAD_BUTTON_B: return GamepadUIKey::B;
            case D_GAMEPAD_BUTTON_X: return GamepadUIKey::X;
            case D_GAMEPAD_BUTTON_Y: return GamepadUIKey::Y;
            case D_GAMEPAD_BUTTON_LEFT_BUMPER: return GamepadUIKey::LEFT_SHOULDER;
            case D_GAMEPAD_BUTTON_RIGHT_BUMPER: return GamepadUIKey::RIGHT_SHOULDER;
            case D_GAMEPAD_BUTTON_BACK: return GamepadUIKey::VIEW;
            case D_GAMEPAD_BUTTON_START: return GamepadUIKey::MENU;
            case D_GAMEPAD_BUTTON_GUIDE: return GamepadUIKey::GUIDE;
            case D_GAMEPAD_BUTTON_LEFT_THUMB: return GamepadUIKey::LEFT_THUMB;
            case D_GAMEPAD_BUTTON_RIGHT_THUMB: return GamepadUIKey::RIGHT_THUMB;
            case D_GAMEPAD_BUTTON_DPAD_UP: return GamepadUIKey::DPAD_UP;
            case D_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadUIKey::DPAD_RIGHT;
            case D_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadUIKey::DPAD_DOWN;
            case D_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadUIKey::DPAD_LEFT;
            default: return GamepadUIKey::COUNT;
        }
    }

    bool isGamepadDirectionKey(GamepadUIKey key){
        return key >= GamepadUIKey::DPAD_UP && key <= GamepadUIKey::LEFT_STICK_RIGHT;
    }

    bool isGamepadTrigger(int axis){
        return axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }

    // ---- the component UI's selection (ui-components spec 6.5): keys and the gamepad reach it only
    // while a Button is selected, so a visible HUD button never takes them

    // the keys whose press the selection took: their release is the UI's too (whatever the press
    // did: a hold, e:use(), a scene its onClick removed)
    std::set<int> navigationKeysDown;
    // the character a Submit key's press types next (Space, Enter's return): not the game's
    uint32_t navigationSwallowedChar = 0;

    // the top scene (layer order) whose component UI holds a live selection; null: none
    Scene* selectionScene(const std::vector<Scene*>& scenes){
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if (canSendUIEvents(*it) && (*it)->getSystem<UISystem>()->hasLiveSelection())
                return *it;
        }
        return nullptr;
    }

    // a component InputField being typed in, top layer first; null: none
    Scene* componentTypingScene(const std::vector<Scene*>& scenes){
        for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
            if (canSendUIEvents(*it) && (*it)->getSystem<UISystem>()->isInputFieldTyping())
                return *it;
        }
        return nullptr;
    }

    bool documentTyping(const std::vector<Scene*>& scenes){
        for (Scene* scene : scenes){
            if (canSendDocumentEvents(scene) && scene->getSystem<UIDocumentSystem>()->hasFocusedTextInput())
                return true;
        }
        return false;
    }

    // a key of a live selection: arrows move, Tab / Shift+Tab, Enter / keypad Enter / Space
    // submit, Escape cancels (when a script handles it). Ctrl, Alt or Super held: the game's
    bool keyToNavigation(const std::vector<Scene*>& scenes, int key, bool repeat, int mods){
        if (mods & (D_MODIFIER_CONTROL | D_MODIFIER_ALT | D_MODIFIER_SUPER))
            return false;
        if ((mods & D_MODIFIER_SHIFT) && key != D_KEY_TAB)
            return false;
        bool navigation = false;
        switch (key){
            case D_KEY_UP: case D_KEY_DOWN: case D_KEY_LEFT: case D_KEY_RIGHT:
            case D_KEY_TAB: case D_KEY_ENTER: case D_KEY_KP_ENTER: case D_KEY_SPACE: case D_KEY_ESCAPE:
                navigation = true;
                break;
            default:
                break;
        }
        if (!navigation)
            return false;
        Scene* target = selectionScene(scenes);
        if (!target)
            return false;
        UISystem* ui = target->getSystem<UISystem>().get();
        switch (key){
            case D_KEY_UP: return ui->eventOnMove(MoveDirection::UP);
            case D_KEY_DOWN: return ui->eventOnMove(MoveDirection::DOWN);
            case D_KEY_LEFT: return ui->eventOnMove(MoveDirection::LEFT);
            case D_KEY_RIGHT: return ui->eventOnMove(MoveDirection::RIGHT);
            case D_KEY_TAB: return ui->eventOnTab((mods & D_MODIFIER_SHIFT) != 0);
            case D_KEY_ESCAPE: return !repeat && ui->eventOnCancel();
            default:
                // a held Submit key repeats nothing, but stays the UI's
                return repeat ? true : ui->eventOnSubmitDown(key);
        }
    }

    MoveDirection gamepadMoveDirection(GamepadUIKey key){
        switch (key){
            case GamepadUIKey::DPAD_UP: case GamepadUIKey::LEFT_STICK_UP: return MoveDirection::UP;
            case GamepadUIKey::DPAD_DOWN: case GamepadUIKey::LEFT_STICK_DOWN: return MoveDirection::DOWN;
            case GamepadUIKey::DPAD_LEFT: case GamepadUIKey::LEFT_STICK_LEFT: return MoveDirection::LEFT;
            case GamepadUIKey::DPAD_RIGHT: case GamepadUIKey::LEFT_STICK_RIGHT: return MoveDirection::RIGHT;
            default: return MoveDirection::NONE;
        }
    }

    int gamepadSubmitSource(int gamepad, GamepadUIKey key){
        return 1000 + gamepad * 32 + static_cast<int>(key);
    }

    // a gamepad key of a live selection: directions move, A submits, B cancels (when handled)
    bool gamepadKeyToNavigation(const std::vector<Scene*>& scenes, int gamepad, GamepadUIKey key){
        const MoveDirection dir = gamepadMoveDirection(key);
        if (dir == MoveDirection::NONE && key != GamepadUIKey::A && key != GamepadUIKey::B)
            return false;
        // a field typed in keeps its keys; the pad does not move a menu meanwhile
        if (componentTypingScene(scenes))
            return false;
        Scene* target = selectionScene(scenes);
        if (!target)
            return false;
        UISystem* ui = target->getSystem<UISystem>().get();
        if (dir != MoveDirection::NONE)
            return ui->eventOnMove(dir);
        if (key == GamepadUIKey::A)
            return ui->eventOnSubmitDown(gamepadSubmitSource(gamepad, key));
        return ui->eventOnCancel();
    }

    GamepadUIHeld* findGamepadHeld(int gamepad, GamepadUIKey key){
        for (GamepadUIHeld& held : gamepadUI.held){
            if (held.gamepad == gamepad && held.key == key)
                return &held;
        }
        return nullptr;
    }

    // the focused document first, as a key: the first scene (top layer first) whose documents consume it
    bool gamepadKeyDown(const std::vector<Scene*>& scenes, int gamepad, GamepadUIKey key){
        if (key == GamepadUIKey::COUNT)
            return false;
        // a down without an up between (a platform repeating it) is a repeat
        if (GamepadUIHeld* held = findGamepadHeld(gamepad, key)){
            if (held->owner == GamepadUIOwner::DOCUMENTS || held->owner == GamepadUIOwner::NONE){
                dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
                    return documents->eventOnGamepadKeyDown(gamepad, key, true);
                });
            }
            return held->owner != GamepadUIOwner::NONE;
        }
        GamepadUIOwner owner = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnGamepadKeyDown(gamepad, key, false);
        }) != nullptr ? GamepadUIOwner::DOCUMENTS : GamepadUIOwner::NONE;
        // then the component UI's selection
        if (owner == GamepadUIOwner::NONE && gamepadKeyToNavigation(scenes, gamepad, key))
            owner = GamepadUIOwner::COMPONENT;
        gamepadUI.held.push_back(GamepadUIHeld{gamepad, key, owner, HtmlUIClock::now() + gamepadRepeatDelay});
        return owner != GamepadUIOwner::NONE;
    }

    // every scene's documents hear the up (each consumes the ups of the downs it consumed), as key ups
    bool gamepadKeyUp(const std::vector<Scene*>& scenes, int gamepad, GamepadUIKey key){
        if (key == GamepadUIKey::COUNT)
            return false;
        GamepadUIOwner owner = GamepadUIOwner::NONE;
        for (auto it = gamepadUI.held.begin(); it != gamepadUI.held.end(); ++it){
            if (it->gamepad == gamepad && it->key == key){
                owner = it->owner;
                gamepadUI.held.erase(it);
                break;
            }
        }
        bool consumed = false;
        for (Scene* scene : scenes){
            if (scene->getSystem<UIDocumentSystem>()->eventOnGamepadKeyUp(gamepad, key))
                consumed = true;
        }
        if (owner == GamepadUIOwner::COMPONENT){
            // the up of a down the selection took: its Submit hold ends where it is
            if (key == GamepadUIKey::A){
                for (Scene* scene : scenes)
                    scene->getSystem<UISystem>()->eventOnSubmitUp(gamepadSubmitSource(gamepad, key));
            }
            consumed = true;
        }
        return consumed;
    }

    // the left stick's direction: the one held stays while its component is past the release
    // threshold; else the dominant axis past the press threshold
    GamepadUIKey stickDirection(float x, float y, GamepadUIKey current){
        switch (current){
            case GamepadUIKey::LEFT_STICK_UP: if (-y >= stickReleaseThreshold) return current; break;
            case GamepadUIKey::LEFT_STICK_DOWN: if (y >= stickReleaseThreshold) return current; break;
            case GamepadUIKey::LEFT_STICK_LEFT: if (-x >= stickReleaseThreshold) return current; break;
            case GamepadUIKey::LEFT_STICK_RIGHT: if (x >= stickReleaseThreshold) return current; break;
            default: break;
        }
        if (std::max(std::fabs(x), std::fabs(y)) < stickPressThreshold)
            return GamepadUIKey::COUNT;
        if (std::fabs(x) >= std::fabs(y))
            return (x > 0.0f) ? GamepadUIKey::LEFT_STICK_RIGHT : GamepadUIKey::LEFT_STICK_LEFT;
        return (y > 0.0f) ? GamepadUIKey::LEFT_STICK_DOWN : GamepadUIKey::LEFT_STICK_UP;
    }

    bool documentsHaveGamepadFocus(const std::vector<Scene*>& scenes){
        for (Scene* scene : scenes){
            if (canSendDocumentEvents(scene) && scene->getSystem<UIDocumentSystem>()->hasGamepadFocus())
                return true;
        }
        return false;
    }

    // an axis move as keys; true when the documents take the axis
    bool gamepadAxisToDocuments(const std::vector<Scene*>& scenes, int gamepad, int axis, float value){
        if (axis == D_GAMEPAD_AXIS_LEFT_X || axis == D_GAMEPAD_AXIS_LEFT_Y){
            auto it = gamepadUI.stick.find(gamepad);
            const GamepadUIKey current = (it != gamepadUI.stick.end()) ? it->second : GamepadUIKey::COUNT;
            const GamepadUIKey next = stickDirection(Input::getGamepadAxis(gamepad, D_GAMEPAD_AXIS_LEFT_X),
                                                     Input::getGamepadAxis(gamepad, D_GAMEPAD_AXIS_LEFT_Y), current);
            if (next != current){
                gamepadUI.stick[gamepad] = next;
                gamepadKeyUp(scenes, gamepad, current);
                gamepadKeyDown(scenes, gamepad, next);
            }
            // a focused document or a selected Button takes the stick, so the player does not
            // move while a menu is used
            const GamepadUIHeld* held = findGamepadHeld(gamepad, next);
            return (held && held->owner != GamepadUIOwner::NONE) || documentsHaveGamepadFocus(scenes) ||
                   (!componentTypingScene(scenes) && selectionScene(scenes) != nullptr);
        }
        if (isGamepadTrigger(axis)){
            const GamepadUIKey key = (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER) ? GamepadUIKey::LEFT_TRIGGER : GamepadUIKey::RIGHT_TRIGGER;
            const GamepadUIHeld* held = findGamepadHeld(gamepad, key);
            if (!held && value > triggerPressThreshold)
                gamepadKeyDown(scenes, gamepad, key);
            else if (held && value < triggerReleaseThreshold)
                gamepadKeyUp(scenes, gamepad, key);
            held = findGamepadHeld(gamepad, key);
            return held && held->owner != GamepadUIOwner::NONE;
        }
        return false;
    }

}

void Engine::systemTouchStart(int pointer, float x, float y){
    // the debug overlay's gestures (three fingers held) see every finger; never consumed
    DebugOverlay::handleTouch(pointer, x, y, 0);
    if (transformCoordPos(x, y)){
#if JASPERENGINE_AUDIO_CAPTURE
        if (AudioMarkers::isEnabled()) AudioMarkers::input("touch", "down", pointer, x, y);
#endif
        Input::addTouch(pointer, x, y);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        // screen documents are in front of the component UI (one they consume does not reach
        // it), which is in front of the documents on meshes
        Scene* documentScene = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnPointerDown(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), true, false);
        });
        if (documentScene)
            uiEventReceived = true;

        // per pointer: a finger a document took never reaches the component UI (ui-multitouch)
        if (!documentScene){
            for (Scene* scene : scenes){
                if (canSendUIEvents(scene))
                    if (scene->getSystem<UISystem>()->eventOnPointerDown(pointer, D_MOUSE_BUTTON_1, x, y, true))
                        uiEventReceived = true;
            }
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), PointerPhase::DOWN, true, covered);
            }))
            uiEventReceived = true;
        if (uiEventReceived)
            Input::setTouchConsumedByUI(pointer);

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onTouchStart.call(pointer, x, y);
            if (Engine::isCallMouseInTouchEvent()){
                // the simulated mouse is one device: a finger multiTouchEnabled hides does not press it
                if (Input::isTouchVisible(pointer))
                    Input::addMousePressed(D_MOUSE_BUTTON_1);
                // the finger is where the pointer jumps to, not a motion
                Input::warpMouse(x, y);
                Engine::onMouseDown.call(D_MOUSE_BUTTON_1, x, y, 0);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemTouchEnd(int pointer, float x, float y){
    DebugOverlay::handleTouch(pointer, x, y, 2);
    // a finger Input.multiTouchEnabled hides does not move or press the simulated mouse
    const bool visible = Input::isTouchVisible(pointer);
    const bool onCanvas = transformCoordPos(x, y);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::input("touch", "up", pointer, x, y);
#endif
    if (!onCanvas){
        // lifted outside the canvas: no event, but the finger is not down any more, and the
        // documents and the component UI that tracked it let go of it without a click (D6)
        Input::removeTouch(pointer);
        if (Engine::isCallMouseInTouchEvent() && visible)
            Input::releaseMousePressed(D_MOUSE_BUTTON_1);
        cancelUIPointer(pointer, true);
        return;
    }
    {
        Input::setTouchPosition(pointer, x, y);
        Input::removeTouch(pointer);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        if (dispatchReleaseToDocuments(scenes, pointer, D_MOUSE_BUTTON_1, true, [&](UIDocumentSystem* documents){
                return documents->eventOnPointerUp(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), true, false);
            }))
            uiEventReceived = true;

        for (Scene* scene : scenes){
            // a covered scene still gets the release
            if (scene->canReceiveUIEvents())
                if (scene->getSystem<UISystem>()->eventOnPointerUp(pointer, D_MOUSE_BUTTON_1, x, y, true))
                    uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, true, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), PointerPhase::UP, true, covered);
            }))
            uiEventReceived = true;

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onTouchEnd.call(pointer, x, y);
            if (Engine::isCallMouseInTouchEvent()){
                if (visible)
                    Input::releaseMousePressed(D_MOUSE_BUTTON_1);
                Input::setMousePosition(x, y);
                Engine::onMouseUp.call(D_MOUSE_BUTTON_1, x, y, 0);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemTouchMove(int pointer, float x, float y){
    DebugOverlay::handleTouch(pointer, x, y, 1);
    if (transformCoordPos(x, y)){
        Input::setTouchPosition(pointer, x, y);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        // only the document a finger started on tracks it, so every scene is offered the move
        for (UIDocumentSystem* documents : documentSystemsTopFirst(scenes)){
            if (documents->eventOnPointerMove(pointer, x, y, Input::getModifiers(), true, false))
                uiEventReceived = true;
        }

        for (Scene* scene : scenes){
            if (canSendUIEvents(scene))
                if (scene->getSystem<UISystem>()->eventOnPointerMove(pointer, x, y, true))
                    uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(pointer, -1, x, y, Input::getModifiers(), PointerPhase::MOVE, true, covered);
            }))
            uiEventReceived = true;

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onTouchMove.call(pointer, x, y);
            if (Engine::isCallMouseInTouchEvent()){
                if (Input::isTouchVisible(pointer))
                    Input::moveMouse(x, y);
                Engine::onMouseMove.call(x, y, 0);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::cancelUIPointer(int pointerOrButton, bool touch){
    for (Scene* scene : getInputScenes(false)){
        if (touch){
            scene->getSystem<UIDocumentSystem>()->eventOnTouchCancel(pointerOrButton);
            scene->getSystem<UISystem>()->eventOnPointerCancel(pointerOrButton);
        }else{
            scene->getSystem<UIDocumentSystem>()->eventOnMouseCancel(pointerOrButton);
            scene->getSystem<UISystem>()->eventOnPointerCancel(-1 - pointerOrButton);
        }
    }
}

void Engine::cancelAllUIPointers(){
    // the debug overlay's gestures too: no finger stays held across a pause or a focus loss
    DebugOverlay::cancelGestures();
    for (Scene* scene : getInputScenes(false)){
        scene->getSystem<UIDocumentSystem>()->cancelAllPointers();
        scene->getSystem<UISystem>()->cancelAllPointers();
    }
}

void Engine::systemTouchCancel(){
    std::vector<Touch> cancelled = Input::getTouches();
    for (const Touch& touch : cancelled)
        systemTouchCancel(touch.pointer);
    // the debug overlay's fingers outside the canvas, which Input never had
    DebugOverlay::cancelGestures();
}

void Engine::systemTouchCancel(int pointer){
    // the debug overlay sees every finger, also those outside the canvas that Input never had
    DebugOverlay::handleTouch(pointer, 0.0f, 0.0f, 3);
    // Drop this finger only. Pointer-up would release the control and can click it.
    size_t index = Input::findTouchIndex(pointer);
    if (index == -1)
        return;
    Vector2 pos = Input::getTouchPosition(pointer);
    // a cancelled finger that simulated the mouse lets go of it too
    if (Engine::isCallMouseInTouchEvent() && Input::isTouchVisible(pointer))
        Input::releaseMousePressed(D_MOUSE_BUTTON_1);
    Input::cancelTouch(pointer);

    // the documents and the component UI that tracked it let go of it without a click
    bool consumed = false;
    for (Scene* scene : getInputScenes(false)){
        if (scene->getSystem<UIDocumentSystem>()->eventOnTouchCancel(pointer))
            consumed = true;
        if (scene->canReceiveUIEvents() && scene->getSystem<UISystem>()->eventOnPointerCancel(pointer))
            consumed = true;
    }

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onTouchCancel.call(pointer, pos.x, pos.y);
}

void Engine::systemMouseDown(int button, float x, float y, int mods){
    if (transformCoordPos(x, y)){
#if JASPERENGINE_AUDIO_CAPTURE
        if (AudioMarkers::isEnabled()) AudioMarkers::input("mouse", "down", button, x, y);
#endif
        Input::addMousePressed(button, mods);
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        // screen documents are in front of the component UI (one they consume does not reach
        // it), which is in front of the documents on meshes
        Scene* documentScene = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnPointerDown(0, button, x, y, mods, false, false);
        });
        if (documentScene)
            uiEventReceived = true;

        // the mouse's buttons are pointers -1, -2, -3 (Unity); the built-in widgets take the
        // left one only. A press a document took never reaches the component UI.
        if (!documentScene && button >= D_MOUSE_BUTTON_LEFT && button <= D_MOUSE_BUTTON_MIDDLE){
            for (Scene* scene : scenes){
                if (canSendUIEvents(scene))
                    if (scene->getSystem<UISystem>()->eventOnPointerDown(-1 - button, button, x, y, false))
                        uiEventReceived = true;
            }
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(0, button, x, y, mods, PointerPhase::DOWN, false, covered);
            }))
            uiEventReceived = true;
        if (uiEventReceived)
            Input::setPressConsumedByUI(Input::mouseControl(button));

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onMouseDown.call(button, x, y, mods);
            if (Engine::isCallTouchInMouseEvent()){
                Input::addTouch(0, x, y);
                Engine::onTouchStart.call(0, x, y);
            }
        }
        uiEventReceived = false;
    }
}
void Engine::systemMouseUp(int button, float x, float y, int mods){
    const bool onCanvas = transformCoordPos(x, y);
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::input("mouse", "up", button, x, y);
#endif
    if (!onCanvas){
        // released outside the canvas: no event, but the button is not held any more, and the
        // documents and the component UI that tracked it let go of it without a click (D6)
        Input::releaseMousePressed(button);
        if (Engine::isCallTouchInMouseEvent())
            Input::removeTouch(0);
        cancelUIPointer(button, false);
        return;
    }
    {
        Input::releaseMousePressed(button);
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        if (dispatchReleaseToDocuments(scenes, 0, button, false, [&](UIDocumentSystem* documents){
                return documents->eventOnPointerUp(0, button, x, y, mods, false, false);
            }))
            uiEventReceived = true;

        for (Scene* scene : scenes){
            // a covered scene still gets the release, like the component UI
            if (scene->canReceiveUIEvents())
                if (button >= D_MOUSE_BUTTON_LEFT && button <= D_MOUSE_BUTTON_MIDDLE)
                    if (scene->getSystem<UISystem>()->eventOnPointerUp(-1 - button, button, x, y, false))
                        uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, true, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(0, button, x, y, mods, PointerPhase::UP, false, covered);
            }))
            uiEventReceived = true;

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onMouseUp.call(button, x, y, mods);
            if (Engine::isCallTouchInMouseEvent()){
                Input::removeTouch(0);
                Engine::onTouchEnd.call(0, x, y);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemMouseMove(float x, float y, int mods){
    // the movement counts outside the canvas too (a captured or dragged mouse looks around)
    const bool insideCanvas = transformCoordPos(x, y);
    Input::addMouseMovement(x, y);
    if (insideCanvas){
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getInputScenes(false);

        // Below the document the pointer is over, documents lose their hover (unless they hold
        // a press), as elements under an opaque one do in a browser
        UIDocumentSystem* cursorDocuments = nullptr;
        for (UIDocumentSystem* documents : documentSystemsTopFirst(scenes)){
            if (cursorDocuments && !documents->hasPointerCapture(0, D_MOUSE_BUTTON_1, false)){
                documents->eventOnMouseLeave();
                continue;
            }
            if (documents->eventOnPointerMove(0, x, y, mods, false, false)){
                if (!cursorDocuments)
                    cursorDocuments = documents;
                uiEventReceived = true;
            }
        }

        // over a component UI element (hovered, not only pressed) the world is covered
        bool overComponentUI = false;
        for (Scene* scene : scenes){
            if (canSendUIEvents(scene)){
                UISystem* ui = scene->getSystem<UISystem>().get();
                if (ui->eventOnPointerMove(-1, x, y, false))
                    uiEventReceived = true;
                if (ui->isPointerOverUI(-1))
                    overComponentUI = true;
            }
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived || overComponentUI, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(0, -1, x, y, mods, PointerPhase::MOVE, false, covered);
            }))
            uiEventReceived = true;

        // the component UI sets its cursor on every move; a document in front of it wins
        if (cursorDocuments){
            Engine::setMouseCursor(cursorDocuments->getPointerCursor());
            documentCursorSet = true;
        }else if (documentCursorSet){
            documentCursorSet = false;
            if (mouseCursorType != CursorType::ARROW && !std::any_of(scenes.begin(), scenes.end(), canSendUIEvents))
                Engine::setMouseCursor(CursorType::ARROW);
        }

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onMouseMove.call(x, y, mods);
            if (Engine::isCallTouchInMouseEvent()){
                if (Input::isMousePressed(D_MOUSE_BUTTON_LEFT) || Input::isMousePressed(D_MOUSE_BUTTON_RIGHT)){
                    Input::setTouchPosition(0, x, y);
                    Engine::onTouchMove.call(0, x, y);
                }
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemMouseScroll(float xoffset, float yoffset, int mods){
    Input::setMouseScroll(xoffset, yoffset);
    if (mods != 0)
        Input::setModifiers(mods);

    const std::vector<Scene*> scenes = getInputScenes(false);
    bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
        return documents->eventOnWheel(xoffset, yoffset, mods);
    }) != nullptr;
    if (!consumed)
        Input::addMouseScrollForGame(xoffset, yoffset);

    // then onScroll of the component UI element under the mouse (top layer first)
    for (auto it = scenes.rbegin(); it != scenes.rend() && !consumed; ++it){
        if (canSendUIEvents(*it) && (*it)->getSystem<UISystem>()->eventOnScroll(xoffset, yoffset))
            consumed = true;
    }

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onMouseScroll.call(xoffset, yoffset, mods);
}

void Engine::systemMouseEnter(){
    Input::addMouseEntered();
    // documents hover again with the next move
    Engine::onMouseEnter.call();
}

void Engine::systemMouseLeave(){
    Input::releaseMouseEntered();
    for (Scene* scene : getInputScenes(false)){
        scene->getSystem<UIDocumentSystem>()->eventOnMouseLeave();
        scene->getSystem<UISystem>()->eventOnPointerLeave(-1);
    }
    Engine::onMouseLeave.call();
}

void Engine::systemKeyDown(int key, bool repeat, int mods){
#if JASPERENGINE_AUDIO_CAPTURE
    if (!repeat && AudioMarkers::isEnabled()) AudioMarkers::input("key", "down", key, 0.0f, 0.0f);
#endif
    Input::addKeyPressed(key, repeat, mods);
    if (mods != 0)
        Input::setModifiers(mods);

    // the debug overlay's toggle and stats keys; never consumed
    DebugOverlay::handleKey(key, mods, true, repeat);

    const std::vector<Scene*> scenes = getInputScenes(true);

    // a component InputField typed in, in any scene, takes the keys first: all but Escape and
    // F1-F25 are its (the player does not walk while typing; documents' text fields do the same)
    bool consumed = false;
    Scene* typing = componentTypingScene(scenes);
    if (typing){
        typing->getSystem<UISystem>()->eventOnKeyDown(key, repeat, mods);
        consumed = key != D_KEY_ESCAPE && !(key >= D_KEY_F1 && key <= D_KEY_F25);
    }

    // then the documents: one with a text field being typed in first (an upper document's
    // focused button must not take Space or Enter from it), then top layer first
    const bool docTyping = documentTyping(scenes);
    if (!consumed){
        Scene* typingDocument = nullptr;
        if (docTyping){
            for (auto it = scenes.rbegin(); it != scenes.rend() && !typingDocument; ++it){
                if (canSendDocumentEvents(*it) && (*it)->getSystem<UIDocumentSystem>()->hasFocusedTextInput())
                    typingDocument = *it;
            }
            if (typingDocument && typingDocument->getSystem<UIDocumentSystem>()->eventOnKeyDown(key, repeat, mods))
                consumed = true;
        }
        // while a field is typed in, the other documents get only what typing leaves (Escape, F1-F25)
        const bool typingKey = key != D_KEY_ESCAPE && !(key >= D_KEY_F1 && key <= D_KEY_F25);
        if (!consumed && !(typingDocument && typingKey)){
            const UIDocumentSystem* asked = typingDocument ? typingDocument->getSystem<UIDocumentSystem>().get() : nullptr;
            consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
                return documents != asked && documents->eventOnKeyDown(key, repeat, mods);
            }) != nullptr;
        }
    }

    // then the component UI's selection (a keyboard menu), never while something is typed in
    navigationSwallowedChar = 0;
    if (!consumed && !typing && !docTyping && keyToNavigation(scenes, key, repeat, mods)){
        consumed = true;
        navigationKeysDown.insert(key);
        if (key == D_KEY_SPACE)
            navigationSwallowedChar = ' ';
        else if (key == D_KEY_ENTER || key == D_KEY_KP_ENTER)
            navigationSwallowedChar = '\r';
    }

    if (consumed && !repeat)
        Input::setPressConsumedByUI(Input::keyControl(key));

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onKeyDown.call(key, repeat, mods);
}

void Engine::systemKeyUp(int key, bool repeat, int mods){
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::input("key", "up", key, 0.0f, 0.0f);
#endif
    Input::releaseKeyPressed(key);
    Input::setModifiers(mods); // Now it can be 0

    // every document hears the release; the one that took the press consumes it. While a text
    // field is typed in, the other documents do not hear a typing key's release either (a focused
    // button clicks on Space's release)
    const std::vector<Scene*> scenes = getInputScenes(false);
    const bool typingKey = key != D_KEY_ESCAPE && !(key >= D_KEY_F1 && key <= D_KEY_F25);
    const bool docTyping = typingKey && documentTyping(scenes);
    bool consumed = false;
    for (Scene* scene : scenes){
        UIDocumentSystem* documents = scene->getSystem<UIDocumentSystem>().get();
        const bool hears = !docTyping || (canSendDocumentEvents(scene) && documents->hasFocusedTextInput());
        if (hears && documents->eventOnKeyUp(key, mods))
            consumed = true;
        // the release of a Submit key that holds a Button pressed
        if ((key == D_KEY_ENTER || key == D_KEY_KP_ENTER || key == D_KEY_SPACE) &&
            scene->getSystem<UISystem>()->eventOnSubmitUp(key))
            consumed = true;
    }

    if (navigationKeysDown.erase(key))
        consumed = true;

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onKeyUp.call(key, repeat, mods);
}

void Engine::systemCharInput(char32_t unit){
    std::vector<uint32_t> codepoints;
    charInputCombiner.push(static_cast<uint32_t>(unit), codepoints);
    // half of a pair: nothing until its other half
    if (!codepoints.empty())
        deliverCodepoints(codepoints);
}

void Engine::flushCharInput(){
    std::vector<uint32_t> codepoints;
    charInputCombiner.flush(codepoints);
    // a high surrogate whose low one never came: U+FFFD, everywhere a character goes
    if (!codepoints.empty())
        deliverCodepoints(codepoints);
}

void Engine::deliverCodepoints(const std::vector<uint32_t>& input){
    const std::vector<Scene*> scenes = getInputScenes(true);

    // the character of a key the selection submitted with is not typed anywhere
    std::vector<uint32_t> codepoints = input;
    if (navigationSwallowedChar != 0 && !codepoints.empty() &&
        (codepoints.front() == navigationSwallowedChar || (navigationSwallowedChar == '\r' && codepoints.front() == '\n'))){
        codepoints.erase(codepoints.begin());
        navigationSwallowedChar = 0;
        if (codepoints.empty())
            return;
    }

    // Input.inputString (Unity's: the text typed this frame)
    for (uint32_t typed : codepoints)
        Input::addInputCharacter(typed);

    // a component InputField typed in takes the characters (the typing guard)
    if (Scene* typing = componentTypingScene(scenes)){
        for (uint32_t codepoint : codepoints){
            typing->getSystem<UISystem>()->eventOnCharInput(static_cast<char32_t>(codepoint));
            if (!ignoreEventsHandledByUI)
                Engine::onCharInput.call(static_cast<char32_t>(codepoint));
        }
        return;
    }

    const std::string utf8 = toUtf8(codepoints);
    const bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
        return documents->eventOnTextInput(utf8);
    }) != nullptr;

    for (uint32_t codepoint : codepoints)
        sendCharacterToGame(codepoint, !consumed, !ignoreEventsHandledByUI || !consumed, scenes);
}

void Engine::systemTextInput(const std::string& typed){
    // the character of a key the selection submitted with is not typed anywhere
    std::string utf8 = typed;
    if (navigationSwallowedChar != 0 && !utf8.empty() && (unsigned char)utf8[0] == navigationSwallowedChar){
        utf8.erase(0, 1);
        navigationSwallowedChar = 0;
    }
    if (utf8.empty())
        return;

    const std::vector<Scene*> scenes = getInputScenes(true);

    bool hadInvalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(utf8, hadInvalid);
    // Input.inputString
    for (uint32_t codepoint : codepoints)
        Input::addInputCharacter(codepoint);

    // a component InputField typed in takes the text (the typing guard)
    if (Scene* typing = componentTypingScene(scenes)){
        for (uint32_t codepoint : codepoints){
            typing->getSystem<UISystem>()->eventOnCharInput(static_cast<char32_t>(codepoint));
            if (!ignoreEventsHandledByUI)
                Engine::onCharInput.call(static_cast<char32_t>(codepoint));
        }
        return;
    }

    const bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
        return documents->eventOnTextInput(utf8);
    }) != nullptr;

    if (ignoreEventsHandledByUI && consumed)
        return;

    for (uint32_t codepoint : codepoints)
        sendCharacterToGame(codepoint, !consumed, true, scenes);
}

void Engine::systemImeComposition(const std::string& utf8, int cursorCodepoints){
    dispatchToDocuments(getInputScenes(true), [&](UIDocumentSystem* documents){
        return documents->eventOnImeComposition(utf8, cursorCodepoints);
    });
}

void Engine::systemImeCancel(){
    for (Scene* scene : getInputScenes(false))
        scene->getSystem<UIDocumentSystem>()->eventOnImeCancel();
}

bool Engine::wantsTextComposition(){
    for (Scene* scene : getInputScenes(false)){
        if (canSendDocumentEvents(scene) && scene->getSystem<UIDocumentSystem>()->hasFocusedTextInput())
            return true;
    }
    return false;
}

void Engine::setTextInputArea(const Rect& canvasRect){
    const float canvasWidth = (float)Engine::getCanvasWidth();
    const float canvasHeight = (float)Engine::getCanvasHeight();
    if (!viewLoaded || canvasWidth <= 0 || canvasHeight <= 0)
        return;

    // the inverse of transformCoordPos: canvas to the platform's screen pixels
    const float scaleX = viewRect.getWidth() / canvasWidth;
    const float scaleY = viewRect.getHeight() / canvasHeight;
    System::instance().setTextInputArea(viewRect.getX() + canvasRect.getX() * scaleX,
                                        viewRect.getY() + canvasRect.getY() * scaleY,
                                        canvasRect.getWidth() * scaleX,
                                        canvasRect.getHeight() * scaleY);
}

void Engine::systemGamepadConnect(int gamepad, const std::string& name){
    Input::addGamepad(gamepad, name);
    Engine::onGamepadConnect.call(gamepad);
}

void Engine::systemGamepadDisconnect(int gamepad){
    // the documents let go of what the pad held (:active, the keys' ups)
    const std::vector<Scene*> scenes = getScenesSnapshot();
    std::vector<GamepadUIKey> held;
    for (const GamepadUIHeld& entry : gamepadUI.held){
        if (entry.gamepad == gamepad)
            held.push_back(entry.key);
    }
    for (GamepadUIKey key : held)
        gamepadKeyUp(scenes, gamepad, key);
    gamepadUI.stick.erase(gamepad);
    for (auto it = gamepadUI.gameAxes.begin(); it != gamepadUI.gameAxes.end();){
        if (it->first.first == gamepad) it = gamepadUI.gameAxes.erase(it); else ++it;
    }
    // nothing of a pad that went is handed back to the game later
    for (auto it = gamepadUI.takenAxes.begin(); it != gamepadUI.takenAxes.end();){
        if (it->first == gamepad) it = gamepadUI.takenAxes.erase(it); else ++it;
    }

    Input::removeGamepad(gamepad);
    Engine::onGamepadDisconnect.call(gamepad);
}

void Engine::systemGamepadButtonDown(int gamepad, int button){
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::input("gamepad", "down", button, 0.0f, 0.0f);
#endif
    Input::addGamepadButtonPressed(gamepad, button);

    const bool consumed = gamepadKeyDown(getScenesSnapshot(), gamepad, gamepadKeyOfButton(button));
    if (consumed)
        Input::setPressConsumedByUI(Input::gamepadControl(Input::getGamepadSlot(gamepad), button));

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onGamepadButtonDown.call(gamepad, button);
}

void Engine::systemGamepadButtonUp(int gamepad, int button){
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioMarkers::isEnabled()) AudioMarkers::input("gamepad", "up", button, 0.0f, 0.0f);
#endif
    Input::releaseGamepadButtonPressed(gamepad, button);

    // consumed iff its down was, so the game never gets an up without a down
    const bool consumed = gamepadKeyUp(getScenesSnapshot(), gamepad, gamepadKeyOfButton(button));

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onGamepadButtonUp.call(gamepad, button);
}

void Engine::systemGamepadAxisMove(int gamepad, int axis, float value){
    Input::setGamepadAxisValue(gamepad, axis, value);

    const bool consumed = gamepadAxisToDocuments(getScenesSnapshot(), gamepad, axis, value);
    Input::setGamepadAxisConsumedByUI(gamepad, axis, consumed);

    const std::pair<int, int> id(gamepad, axis);
    // the axes the UI holds go back to the game once no UI wants the pad (updateGamepadNavigation)
    if (consumed)
        gamepadUI.takenAxes.insert(id);
    else
        gamepadUI.takenAxes.erase(id);
    if (ignoreEventsHandledByUI && consumed){
        // the game does not keep a value it got before the documents took the axis (a player
        // walking on when a menu opens): it gets the rest value once
        const float rest = isGamepadTrigger(axis) ? -1.0f : 0.0f;
        auto it = gamepadUI.gameAxes.find(id);
        if (it != gamepadUI.gameAxes.end() && it->second != rest){
            it->second = rest;
            Engine::onGamepadAxisMove.call(gamepad, axis, rest);
        }
        return;
    }
    gamepadUI.gameAxes[id] = value;
    Engine::onGamepadAxisMove.call(gamepad, axis, value);
}

void Engine::getUINavigationWants(bool& gamepad, bool& keyboard){
    const std::vector<Scene*> scenes = getScenesSnapshot();
    // a selected Button (the component UI's menu, or one its history gives back); no callbacks here
    keyboard = false;
    for (Scene* scene : scenes){
        if (canSendUIEvents(scene) && scene->getSystem<UISystem>()->hasSelection()){
            keyboard = true;
            break;
        }
    }
    gamepad = keyboard || documentsHaveGamepadFocus(scenes);
}

bool Engine::wantsGamepadNavigation(){
    bool gamepad = false, keyboard = false;
    getUINavigationWants(gamepad, keyboard);
    return gamepad;
}

bool Engine::wantsKeyboardNavigation(){
    bool gamepad = false, keyboard = false;
    getUINavigationWants(gamepad, keyboard);
    return keyboard;
}

bool Engine::isTextInputFocused(){
    for (Scene* scene : getInputScenes(false)){
        if (canSendUIEvents(scene) && scene->getSystem<UISystem>()->isInputFieldFocused())
            return true;
        if (canSendDocumentEvents(scene) && scene->getSystem<UIDocumentSystem>()->hasFocusedTextInput())
            return true;
    }
    return false;
}

void Engine::updateGamepadNavigation(){
    if (gamepadUI.held.empty() && gamepadUI.takenAxes.empty())
        return;

    // no menu wants the pad any more (a document hidden or blurred, a selection gone): the axes
    // the UI held go back to the game as they are now, a stick held still included
    if (!gamepadUI.takenAxes.empty() && !wantsGamepadNavigation()){
        const std::set<std::pair<int, int>> taken = gamepadUI.takenAxes;
        gamepadUI.takenAxes.clear();
        for (GamepadUIHeld& held : gamepadUI.held){
            if (held.key >= GamepadUIKey::LEFT_STICK_UP && held.key <= GamepadUIKey::LEFT_STICK_RIGHT)
                held.owner = GamepadUIOwner::NONE;
        }
        for (const std::pair<int, int>& id : taken){
            const float value = Input::getGamepadAxis(id.first, id.second);
            Input::setGamepadAxisConsumedByUI(id.first, id.second, false);
            auto it = gamepadUI.gameAxes.find(id);
            if (it == gamepadUI.gameAxes.end() || it->second != value){
                gamepadUI.gameAxes[id] = value;
                Engine::onGamepadAxisMove.call(id.first, id.second, value);
            }
        }
    }
    if (gamepadUI.held.empty())
        return;

    const double now = HtmlUIClock::now();
    std::vector<Scene*> scenes;
    // by index: a listener may press or release keys (send_input, Lua)
    for (size_t i = 0; i < gamepadUI.held.size(); i++){
        GamepadUIHeld& held = gamepadUI.held[i];
        if (!isGamepadDirectionKey(held.key) || held.owner == GamepadUIOwner::NONE || now < held.nextRepeat)
            continue;
        held.nextRepeat += gamepadRepeatInterval;
        if (held.nextRepeat <= now)
            held.nextRepeat = now + gamepadRepeatInterval;
        const int gamepad = held.gamepad;
        const GamepadUIKey key = held.key;
        const GamepadUIOwner owner = held.owner;
        if (scenes.empty())
            scenes = getScenesSnapshot();
        if (owner == GamepadUIOwner::COMPONENT){
            // the selection moves again; gone, the repeat does nothing (the up stays the UI's)
            if (!componentTypingScene(scenes)){
                if (Scene* target = selectionScene(scenes))
                    target->getSystem<UISystem>()->eventOnMove(gamepadMoveDirection(key));
            }
            continue;
        }
        dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnGamepadKeyDown(gamepad, key, true);
        });
    }
}
