// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "manager/ObjectManager.h"
#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "Input.h"
#include "io/UserSettings.h"
#include "input/InputActions.h"
#include "render/SystemRender.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "lua.hpp"
#include "manager/SceneManager.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/anim/AnimEngine.h"
#include "util/StringUtils.h"
#include "util/Utf16Combiner.h"
#include "component/ButtonComponent.h"
#include "component/UIComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/InputFieldComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ActionComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/TexturePool.h"
#include "pool/SoundPool.h"
#include "pool/TextureDataPool.h"
#include "pool/ShaderPool.h"
#include "pool/FontPool.h"
#include "pool/ModelPool.h"
#include "pool/MeshLodPool.h"
#include "pool/MeshBufferPool.h"
#include "pool/AnimationClipPool.h"
#include "pool/AvatarMaskPool.h"
#include "pool/AnimatorControllerPool.h"
#include "pool/PhysicsMaterialPool.h"
#include "pool/VertexAnimationPool.h"
#include "shader/RuntimeShaderCompiler.h"
#ifndef NO_THREAD_SUPPORT
    #include "thread/ThreadPoolManager.h"
#endif

#include "sokol_time.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <functional>

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
TextureStrategy Engine::textureStrategy = TextureStrategy::NONE;

bool Engine::callMouseInTouchEvent = false;
bool Engine::callTouchInMouseEvent = false;
bool Engine::useDegrees = true;
bool Engine::allowEventsOutCanvas = false;
bool Engine::ignoreEventsHandledByUI = true;

bool Engine::uiEventReceived = false;

uint64_t Engine::lastTime = 0;
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
uint64_t Engine::gameFrames = 0;
std::atomic<bool> Engine::simulationHeld = false;
bool Engine::clampNextDelta = false;

std::atomic<bool> Engine::viewLoaded = false;
std::atomic<bool> Engine::paused = false;
bool Engine::focused = true;
std::atomic<bool> Engine::asyncLoading = false;
bool Engine::frameRunning = false;

CursorType Engine::mouseCursorType = CursorType::ARROW;
MouseMode Engine::mouseMode = MouseMode::NORMAL;

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
FunctionSubscribe<void(wchar_t)> Engine::onCharInput;
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

Scene* Engine::getLastScene(){
    std::lock_guard<std::mutex> lock(sceneListMutex);
    return scenes.empty() ? nullptr : scenes.back();
}

void Engine::pauseGameEvents(bool pause) {
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
        calculateCanvas();
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
        calculateCanvas();
    }
}

Scaling Engine::getScalingMode(){
    return scalingMode;
}

void Engine::setTextureStrategy(TextureStrategy textureStrategy){
    Engine::textureStrategy = textureStrategy;
}

TextureStrategy Engine::getTextureStrategy(){
    return textureStrategy;
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

void Engine::setIgnoreEventsHandledByUI(bool ignoreEventsHandledByUI){
    Engine::ignoreEventsHandledByUI = ignoreEventsHandledByUI;
}

bool Engine::isIgnoreEventsHandledByUI(){
    return ignoreEventsHandledByUI;
}

bool Engine::isUIEventReceived(){
    return uiEventReceived;
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
    if (viewLoaded){
        System::instance().setMouseMode(mode);
    }
}

MouseMode Engine::getMouseMode(){
    return mouseMode;
}

void Engine::setMousePosition(float x, float y){
    Input::setMousePosition(x, y);

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

float Engine::getDeltatime(){
    return deltatime;
}

void Engine::beginFrameStats(){
    frameStats = currentFrameStats;
    currentFrameStats = FrameStats();
}

void Engine::addDrawStats(uint32_t instances, uint64_t triangles){
    currentFrameStats.drawCalls++;
    currentFrameStats.instances += instances;
    currentFrameStats.triangles += triangles;
}

const Engine::FrameStats& Engine::getFrameStats(){
    return frameStats;
}

float Engine::getMaxDeltatime(){
    return (float)maxDeltatime;
}

void Engine::setMaxDeltatime(float seconds){
    if (seconds > 0.0f)
        maxDeltatime = seconds;
}

double Engine::getSystemTime(){
    return stm_sec(stm_now());
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

double Engine::getGameTime(){
    return gameTime;
}

uint64_t Engine::getGameFrames(){
    return gameFrames;
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

void Engine::setAsyncLoading(bool enable){
    #ifndef NO_THREAD_SUPPORT
        asyncLoading = enable;
    #else
        if (enable){
            Log::warn("Threads are not available");
        }
        asyncLoading = false;
    #endif
}

bool Engine::isAsyncLoading(){
    #ifndef NO_THREAD_SUPPORT
        return asyncLoading;
    #else
        return false;
    #endif
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
    #ifndef NO_THREAD_SUPPORT
        ThreadPoolManager::shutdown();
        ThreadPoolManager::initialize(maxThreads);
    #endif
}

size_t Engine::getQueuedResourceCount() {
    #ifndef NO_THREAD_SUPPORT
        return ThreadPoolManager::getInstance().getQueueSize();
    #else
        return 0;
    #endif
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

int Engine::defaultDescriptorBytes(){
    return (smallGpuMemoryPlatform() ? 16 : 32) * 1024 * 1024;
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
    compositeFramebuffer.create();

    return compositeFramebuffer.isCreated();
}

void Engine::beginCompositeFramebuffer(){
// each scene opens its own swapchain pass and the sokol Vulkan backend takes only one
// per commit, so stacked scenes draw offscreen and are presented once. The other
// backends draw them straight to the swapchain (re-check when sokol_gfx is updated)
#if defined(SOKOL_VULKAN)
    if (framebuffer || scenes.size() <= 1)
        return;

    if (ensureCompositeFramebuffer())
        framebuffer = &compositeFramebuffer;
#endif
}

void Engine::endCompositeFramebuffer(){
    if (framebuffer != &compositeFramebuffer)
        return;

    framebuffer = nullptr;

    if (mainScene)
        mainScene->getSystem<RenderSystem>()->presentFramebufferToSwapchain(&compositeFramebuffer);
}

void Engine::clearPools(){
    // first: its assets' payload textures go back to the texture pools cleared next
    VertexAnimationPool::clear();
    TexturePool::clear();
    SoundPool::clear();
    TextureDataPool::clear();
    ShaderPool::clear();
    FontPool::clear();
    ModelPool::clear();
    MeshLodPool::clear();
    MeshBufferPool::clear();
    AnimationClipPool::clear();
    AvatarMaskPool::clear();
    AnimatorControllerPool::clear();
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
    AvatarMaskPool::clearUnused();
    AnimatorControllerPool::clearUnused();
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
    onSceneLoaded.removeByTagSubstring(substring);

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

    if (includeLifecycle) {
        onViewLoaded.clear();
        onViewChanged.clear();
        onViewDestroyed.clear();
        onDraw.clear();
        onPause.clear();
        onResume.clear();
        onShutdown.clear();
        onFocus.clear();
    }
}

// A script can leave callbacks on components (button.onPress.add(...)) that outlive it, and the
// code behind them goes away with the project library or the Lua state
void Engine::clearComponentSubscriptions(Scene* scene) {
    if (!scene)
        return;

    auto buttons = scene->getComponentArray<ButtonComponent>();
    for (size_t i = 0; i < buttons->size(); i++) {
        ButtonComponent& button = buttons->getComponentFromIndex(i);
        button.onPress.clear();
        button.onRelease.clear();
    }

    auto uis = scene->getComponentArray<UIComponent>();
    for (size_t i = 0; i < uis->size(); i++) {
        UIComponent& ui = uis->getComponentFromIndex(i);
        ui.onGetFocus.clear();
        ui.onLostFocus.clear();
        ui.onPointerEnter.clear();
        ui.onPointerLeave.clear();
        ui.onPointerMove.clear();
        ui.onPointerDown.clear();
        ui.onPointerUp.clear();
        ui.onClick.clear();
        ui.onDoubleClick.clear();
        ui.onDragStart.clear();
        ui.onDrag.clear();
        ui.onDragEnd.clear();
    }

    auto panels = scene->getComponentArray<PanelComponent>();
    for (size_t i = 0; i < panels->size(); i++) {
        PanelComponent& panel = panels->getComponentFromIndex(i);
        panel.onMove.clear();
        panel.onResize.clear();
    }

    auto scrollbars = scene->getComponentArray<ScrollbarComponent>();
    for (size_t i = 0; i < scrollbars->size(); i++) {
        scrollbars->getComponentFromIndex(i).onChange.clear();
    }

    auto inputFields = scene->getComponentArray<InputFieldComponent>();
    for (size_t i = 0; i < inputFields->size(); i++) {
        InputFieldComponent& inputField = inputFields->getComponentFromIndex(i);
        inputField.onChange.clear();
        inputField.onSubmit.clear();
    }

    auto documents = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < documents->size(); i++) {
        UIDocumentComponent& document = documents->getComponentFromIndex(i);
        document.onLoad.clear();
        document.onUnload.clear();
    }

    auto sounds = scene->getComponentArray<AudioSourceComponent>();
    for (size_t i = 0; i < sounds->size(); i++) {
        AudioSourceComponent& sound = sounds->getComponentFromIndex(i);
        sound.onStart.clear();
        sound.onPause.clear();
        sound.onStop.clear();
    }

    auto actions = scene->getComponentArray<ActionComponent>();
    for (size_t i = 0; i < actions->size(); i++) {
        ActionComponent& action = actions->getComponentFromIndex(i);
        action.onStart.clear();
        action.onPause.clear();
        action.onStop.clear();
        action.onStep.clear();
    }

    // contact and activation events a script subscribed to on the scene's physics
    scene->getSystem<PhysicsSystem>()->clearSubscriptions();

    auto animators = scene->getComponentArray<PoseAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++) {
        animators->getComponentFromIndex(i).onFinish.clear();
    }

    auto players = scene->getComponentArray<AnimationPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++) {
        AnimationPlayerComponent& player = players->getComponentFromIndex(i);
        player.onEvent.clear();
        player.onFinish.clear();
        player.onStateEnter.clear();
        player.onStateExit.clear();
    }

    auto vertexAnimators = scene->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < vertexAnimators->size(); i++) {
        VertexAnimatorComponent& animator = vertexAnimators->getComponentFromIndex(i);
        animator.onEvent.clear();
        animator.onFinish.clear();
        animator.onInstanceEvent.clear();
        animator.onInstanceFinish.clear();
    }
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

void Engine::calculateCanvas(){
    Rect unusedViewRect;
    computeViewLayout(System::instance().getScreenWidth(), System::instance().getScreenHeight(),
                      preferredCanvasWidth, preferredCanvasHeight, scalingMode,
                      Engine::canvasWidth, Engine::canvasHeight, unusedViewRect);
}

void Engine::systemInit(int argc, char* argv[], System* system, bool startProject){
    #ifndef NO_THREAD_SUPPORT
        size_t maxThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
        ThreadPoolManager::initialize(maxThreads);
    #endif

    System::setSystemInstance(system);

    Engine::setCanvasSize(1000,480);

    lastTime = 0;
    updateTimeCount = 0;
    viewLoaded = false;
    paused = false;

    drawSemaphore.release();

    stm_setup();
    
    std::vector<std::string> args(argv, argv + argc);
    System::instance().args = args;

    LuaBinding::createLuaState();

    if (startProject)
        systemStartProject();
}

void Engine::systemStartProject(){
    #ifndef NO_LUA_INIT
    LuaBinding::init();
    #endif
    #ifndef NO_CPP_INIT
    getOnInit().call();
    #endif
}

void Engine::systemViewLoaded(){
    SystemRender::setup();

    if (mouseCursorType != CursorType::ARROW){
        System::instance().setMouseCursor(mouseCursorType);
    }
    if (mouseMode != MouseMode::NORMAL){
        System::instance().setMouseMode(mouseMode);
    }

    getAsyncThreadDepthStorage() = 0;

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

    onViewChanged.call();
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

    bool frameStatsEveryFrame(){
        static const bool all = [](){
            const char* value = std::getenv("JASPERENGINE_FRAME_STATS");
            return value && std::string(value) == "all";
        }();
        return all;
    }
}

void Engine::signalReadyOnce(){
    static const bool enabled = [](){
        const char* value = std::getenv("JASPERENGINE_READY_SIGNAL");
        return value && value[0] != '\0' && value[0] != '0';
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
    if (!enabled)
        return;
    if (signalled){
        if (frameStats){
            frames++;
            const uint64_t after = frames - signalledFrame;
            if (frameStatsAll || after == 2 || after == 30 || after == 90)
                SystemRender::logFrameStats(frames);
        }
        return;
    }

    frames++;
    if (frameStatsAll && frames > 1)
        SystemRender::logFrameStats(frames - 1);
    bool ready = ShaderPool::getPendingShaderCount() == 0 && !scenes.empty();
    for (size_t i = 0; ready && i < scenes.size(); i++){
        ready = scenes[i]->getSystem<RenderSystem>()->isAllLoaded();
        // and its HTML UI documents loaded (or failed) and drawn once
        if (ready){
            const auto documents = scenes[i]->getSystem<UIDocumentSystem>();
            ready = !documents || documents->isAllLoaded();
        }
    }
    readyFrames = ready ? readyFrames + 1 : 0;
    if (readyFrames >= 10){
        signalled = true;
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

void Engine::systemUpdate(){
    runFrame(false);
}

void Engine::runFrame(bool draw){
    const int MAX_UPDATES_PER_FRAME = 100;

    //Deltatime in seconds
    double rawDelta = stm_sec(stm_laptime(&lastTime));
    framerate = (rawDelta > 0.0) ? (float)(1.0 / rawDelta) : 0.0f;

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
    // the UI clock runs on the unscaled delta (a pause menu animates at time scale 0) and
    // stops with the updates: a held simulation or a paused app
    HtmlUIClock::advance((paused || simulationHeld) ? 0.0 : frameTime);

    // the input of this frame is over: no low surrogate will follow a pending high one
    flushCharInput();
    // this frame's input: edges, deltas, touches, input actions and their onButtonDown/Up;
    // frames that do not run the update keep it for the next one that does
    const bool runsUpdate = !paused && !simulationHeld;
    if (runsUpdate)
        Input::beginFrame(frameTime);
    // held gamepad directions repeat on the UI clock
    updateGamepadNavigation();

    drawSemaphore.acquire();

    // async uploads of the last frame, while the scenes that queued them still exist
    SystemRender::executeQueue();

    // the main-thread share of runtime shader compilation (one variant a frame when
    // compiling without a worker thread)
    RuntimeShaderCompiler::beginFrame();

    frameRunning = true;

    // what DontDestroyOnLoad / MoveGameObjectToScene asked for between frames (input
    // callbacks, the editor's tools), at this frame's first safe point
    SceneManager::flushPersistentMoves();

    // a transition requested during the last frame; one requested by its factory waits again
    SceneManager::applyPendingLoad();

    // before the update, which bakes the PIP_RTT pipelines from the target in use
    if (draw)
        beginCompositeFramebuffer();

    // avoid increment updateTimeCount after resume; a held simulation only draws
    if (!paused && !simulationHeld) {
        double frameDelta = deltatime;
        gameTime += frameDelta;
        gameFrames++;

        // 0) start of the scripts enabled since the last frame, before their first fixed
        //    update and update (Unity)
        ScriptRuntime::flushStarts();

        // 1) Fixed-timestep phase (deterministic). Runs zero or more times per frame.
        //    Used for physics, networking, and any subsystem overriding fixedUpdate().
        if (updateTime > 0.0) {
            updateTimeCount += frameDelta;

            int fixedLoops = 0;
            while (updateTimeCount >= updateTime && fixedLoops < MAX_UPDATES_PER_FRAME) {
                // counted like this loop, so its last step sees 1
                fixedStepsLeft = 0;
                for (double remaining = updateTimeCount; remaining >= updateTime && fixedLoops + fixedStepsLeft < MAX_UPDATES_PER_FRAME; remaining -= updateTime) {
                    fixedStepsLeft++;
                }

                Engine::onFixedUpdate.call();
                for (int i = 0; i < scenes.size(); i++) {
                    scenes[i]->fixedUpdate(updateTime);
                }
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
        Engine::onUpdate.call();
        for (int i = 0; i < scenes.size(); i++) {
            scenes[i]->update(frameDelta);
        }
        Engine::onPostUpdate.call();

        // GameObject::destroy: after the update, before the draw (Unity)
        ObjectManager::update(frameDelta);
        // their holes in the component arrays closed now, not by whatever reads next
        for (int i = 0; i < scenes.size(); i++) {
            scenes[i]->compactComponents();
        }
    }
    if (runsUpdate)
        Input::endFrame();

    if (draw){
        Engine::onDraw.call();

        // again for a scene stack loaded during the update
        beginCompositeFramebuffer();

        beginFrameStats();
        for (int i = 0; i < scenes.size(); i++){
            scenes[i]->draw();
        }

        endCompositeFramebuffer();
    }

    SystemRender::commit();

    // a one-shot stays until a frame draws it
    if (draw){
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

    // after the draw, so the new scenes are drawn loaded before the loading scene goes
    SceneManager::updateLoading();

    // it counts drawn frames
    if (draw)
        signalReadyOnce();

    // desktop UserSettings: written a second after a change, not on every set
    UserSettings::tick();

    frameRunning = false;

    drawSemaphore.release();
}

#ifdef JASPERENGINE_EDITOR
void Engine::drawView(uint32_t viewId, Framebuffer* target, const std::vector<ViewLayer>& layers){
    if (!viewLoaded || !target || layers.empty())
        return;

    drawSemaphore.acquire();

    // the frame statistics are the game's: a second view of it adds nothing to them
    const FrameStats gameFrameStats = currentFrameStats;

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
        desc.canvasWidth = layer.canvasWidth;
        desc.canvasHeight = layer.canvasHeight;
        layer.scene->getSystem<RenderSystem>()->drawView(viewId, desc);

        baseLayer = false;
    }

    currentFrameStats = gameFrameStats;

    SystemRender::commit();
    // JASPERENGINE_FRAME_STATS=all: this view's passes too (each view commits on its own)
    if (frameStatsEveryFrame())
        SystemRender::logFrameStats(frameStatsFrames, viewId);

    drawSemaphore.release();
}
#endif

void Engine::systemViewDestroyed(){
    SoundPool::requestShutdown();
    TextureDataPool::requestShutdown();

    drawSemaphore.acquire();
    
    viewLoaded = false;
    Engine::onViewDestroyed.call();

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

    #ifndef NO_THREAD_SUPPORT
        ThreadPoolManager::shutdown();
    #endif
}

void Engine::systemShutdown(){
    // Unity's quit: onApplicationQuit to every script, then onDisable and onDestroy (the OS
    // may end a mobile app without this)
    ScriptRuntime::quitAll();

    Engine::onShutdown.call();

    // what the game (and its onShutdown handlers) stored is on disk before it goes
    UserSettings::save();

    // anime's Lua references go while the Lua state still exists
    AnimEngine::instance().reset();
    // the persistent scene's scripts (their onDestroy ran above) are released, and it goes
    SceneManager::destroyPersistentScene();
    LuaBinding::cleanup();

    removeAllSceneLayers(true);

    // and what scripts stored while their scenes went (onDestroy): the last write. Desktop
    // System objects live until the process ends, so ~System never gets to write it.
    UserSettings::save();
}

void Engine::systemPause(){
    AudioSystem::pauseAll();
    ScriptRuntime::broadcast(ScriptMessage::ApplicationPause, [](lua_State* L) { lua_pushboolean(L, 1); return 1; },
        [](ScriptBase& script) { script.onApplicationPause(true); });
    Engine::onPause.call();
    paused = true;
    // the key and button ups of a paused app never come: the first frame after resume sees them
    Input::releaseAll();

    // a paused app may be killed without another call
    UserSettings::save();
}

void Engine::systemResume(){
    AudioSystem::resumeAll();
    ScriptRuntime::broadcast(ScriptMessage::ApplicationPause, [](lua_State* L) { lua_pushboolean(L, 0); return 1; },
        [](ScriptBase& script) { script.onApplicationPause(false); });
    Engine::onResume.call();
    paused = false;
}

void Engine::systemFocus(bool hasFocus){
    if (focused == hasFocus){
        return;
    }
    focused = hasFocus;
    Engine::onFocus.call(hasFocus);
    ScriptRuntime::broadcast(ScriptMessage::ApplicationFocus, [hasFocus](lua_State* L) { lua_pushboolean(L, hasFocus ? 1 : 0); return 1; },
        [hasFocus](ScriptBase& script) { script.onApplicationFocus(hasFocus); });
}

bool Engine::isFocused(){
    return focused;
}

// Scenes below a loading scene only get pointer-up, so nothing stays pressed
static bool canSendUIEvents(Scene* scene){
    return scene->canReceiveUIEvents() && !SceneManager::isCoveredByLoading(scene);
}

// documents are an explicit request for input: they receive it unless the scene turns
// UI events off (UIEventState::DISABLED) or a loading scene covers it
static bool canSendDocumentEvents(Scene* scene){
    return scene->getEnableUIEvents() != UIEventState::DISABLED && !SceneManager::isCoveredByLoading(scene);
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

    // the game's and the component UI's character input: one wchar_t unit per call, as before
    // documents existed (two surrogates where wchar_t is 16 bits)
    void sendCharacterToGame(uint32_t codepoint, bool toComponentUI, bool toGame, const std::vector<Scene*>& scenes){
        std::vector<wchar_t> units;
        if (sizeof(wchar_t) == 2 && codepoint > 0xFFFF){
            const uint32_t value = codepoint - 0x10000u;
            units.push_back((wchar_t)(0xD800u + (value >> 10)));
            units.push_back((wchar_t)(0xDC00u + (value & 0x3FFu)));
        }else{
            units.push_back((wchar_t)codepoint);
        }

        for (wchar_t unit : units){
            if (toGame)
                Engine::onCharInput.call(unit);
            if (toComponentUI){
                for (Scene* scene : scenes){
                    if (canSendUIEvents(scene))
                        scene->getSystem<UISystem>()->eventOnCharInput(unit);
                }
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

    struct GamepadUIHeld {
        int gamepad;
        GamepadUIKey key;
        bool consumed;      // a document took the down
        double nextRepeat;  // UI clock
    };

    struct GamepadUIState {
        std::vector<GamepadUIHeld> held;
        std::map<int, GamepadUIKey> stick;                 // the direction each pad's left stick holds
        std::map<std::pair<int, int>, float> gameAxes;     // the last value of each axis the game got
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
            dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
                return documents->eventOnGamepadKeyDown(gamepad, key, true);
            });
            return held->consumed;
        }
        const bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnGamepadKeyDown(gamepad, key, false);
        }) != nullptr;
        gamepadUI.held.push_back(GamepadUIHeld{gamepad, key, consumed, HtmlUIClock::now() + gamepadRepeatDelay});
        return consumed;
    }

    // every scene's documents hear the up (each consumes the ups of the downs it consumed), as key ups
    bool gamepadKeyUp(const std::vector<Scene*>& scenes, int gamepad, GamepadUIKey key){
        if (key == GamepadUIKey::COUNT)
            return false;
        for (auto it = gamepadUI.held.begin(); it != gamepadUI.held.end(); ++it){
            if (it->gamepad == gamepad && it->key == key){
                gamepadUI.held.erase(it);
                break;
            }
        }
        bool consumed = false;
        for (Scene* scene : scenes){
            if (scene->getSystem<UIDocumentSystem>()->eventOnGamepadKeyUp(gamepad, key))
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
            // a focused document takes the stick, so the player does not move while a menu is used
            const GamepadUIHeld* held = findGamepadHeld(gamepad, next);
            return (held && held->consumed) || documentsHaveGamepadFocus(scenes);
        }
        if (isGamepadTrigger(axis)){
            const GamepadUIKey key = (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER) ? GamepadUIKey::LEFT_TRIGGER : GamepadUIKey::RIGHT_TRIGGER;
            const GamepadUIHeld* held = findGamepadHeld(gamepad, key);
            if (!held && value > triggerPressThreshold)
                gamepadKeyDown(scenes, gamepad, key);
            else if (held && value < triggerReleaseThreshold)
                gamepadKeyUp(scenes, gamepad, key);
            held = findGamepadHeld(gamepad, key);
            return held && held->consumed;
        }
        return false;
    }

}

void Engine::systemTouchStart(int pointer, float x, float y){
    if (transformCoordPos(x, y)){
        Input::addTouch(pointer, x, y);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

        // screen documents are in front of the component UI (one they consume does not reach
        // it), which is in front of the documents on meshes
        Scene* documentScene = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnPointerDown(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), true, false);
        });
        if (documentScene)
            uiEventReceived = true;

        for (Scene* scene : scenes){
            if (scene != documentScene && canSendUIEvents(scene))
                if (scene->getSystem<UISystem>()->eventOnPointerDown(x, y))
                    uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), PointerPhase::DOWN, true, covered);
            }))
            uiEventReceived = true;

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onTouchStart.call(pointer, x, y);
            if (Engine::isCallMouseInTouchEvent()){
                // the simulated mouse is one device: a finger multiTouchEnabled hides does not press it
                if (Input::isTouchVisible(pointer)){
                    Input::addMousePressed(D_MOUSE_BUTTON_1);
                    // it jumps to the finger: no movement
                    Input::resetMouseMovement();
                }
                Input::setMousePosition(x, y);
                Engine::onMouseDown.call(D_MOUSE_BUTTON_1, x, y, 0);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemTouchEnd(int pointer, float x, float y){
    const bool visible = Input::isTouchVisible(pointer);
    if (!transformCoordPos(x, y)){
        // lifted outside the canvas: no event, but the finger is not down any more
        Input::removeTouch(pointer);
        if (Engine::isCallMouseInTouchEvent() && visible)
            Input::releaseMousePressed(D_MOUSE_BUTTON_1);
        return;
    }
    {
        Input::setTouchPosition(pointer, x, y);
        Input::removeTouch(pointer);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

        if (dispatchReleaseToDocuments(scenes, pointer, D_MOUSE_BUTTON_1, true, [&](UIDocumentSystem* documents){
                return documents->eventOnPointerUp(pointer, D_MOUSE_BUTTON_1, x, y, Input::getModifiers(), true, false);
            }))
            uiEventReceived = true;

        for (Scene* scene : scenes){
            // a covered scene still gets the release
            if (scene->canReceiveUIEvents())
                if (scene->getSystem<UISystem>()->eventOnPointerUp(x, y))
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
    if (transformCoordPos(x, y)){
        const bool known = Input::findTouchIndex(pointer) != (size_t)-1;
        const Vector2 previous = Input::getTouchPosition(pointer);
        Input::setTouchPosition(pointer, x, y);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

        // only the document a finger started on tracks it, so every scene is offered the move
        for (UIDocumentSystem* documents : documentSystemsTopFirst(scenes)){
            if (documents->eventOnPointerMove(pointer, x, y, Input::getModifiers(), true, false))
                uiEventReceived = true;
        }

        for (Scene* scene : scenes){
            if (canSendUIEvents(scene))
                if (scene->getSystem<UISystem>()->eventOnPointerMove(x, y))
                    uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(pointer, -1, x, y, Input::getModifiers(), PointerPhase::MOVE, true, covered);
            }))
            uiEventReceived = true;

        if (!ignoreEventsHandledByUI || !uiEventReceived){
            Engine::onTouchMove.call(pointer, x, y);
            if (Engine::isCallMouseInTouchEvent()){
                // the finger moving the simulated mouse moves it (Input.mouseDelta, Mouse X/Y)
                if (known && Input::isTouchVisible(pointer))
                    Input::addMouseDelta(x - previous.x, y - previous.y);
                Input::setMousePosition(x, y);
                Engine::onMouseMove.call(x, y, 0);
            }
        }
        uiEventReceived = false;
    }
}

void Engine::systemTouchCancel(){
    std::vector<Touch> cancelled = Input::getTouches();
    for (const Touch& touch : cancelled)
        systemTouchCancel(touch.fingerId);
}

void Engine::systemTouchCancel(int pointer){
    // Drop this finger only. Pointer-up would release the control and can click it.
    size_t index = Input::findTouchIndex(pointer);
    if (index == -1)
        return;
    Vector2 pos = Input::getTouchPosition(pointer);
    // an interrupted finger that simulated the mouse lets go of it (no mouse up: not a release)
    if (Engine::isCallMouseInTouchEvent() && Input::isTouchVisible(pointer))
        Input::releaseMousePressed(D_MOUSE_BUTTON_1);
    Input::cancelTouch(pointer);

    // the documents that tracked it let go of it without a click
    bool consumed = false;
    for (Scene* scene : getScenesSnapshot()){
        if (scene->getSystem<UIDocumentSystem>()->eventOnTouchCancel(pointer))
            consumed = true;
    }

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onTouchCancel.call(pointer, pos.x, pos.y);
}

void Engine::systemMouseDown(int button, float x, float y, int mods){
    if (transformCoordPos(x, y)){
        Input::addMousePressed(button);
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

        // screen documents are in front of the component UI (one they consume does not reach
        // it), which is in front of the documents on meshes
        Scene* documentScene = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnPointerDown(0, button, x, y, mods, false, false);
        });
        if (documentScene)
            uiEventReceived = true;

        for (Scene* scene : scenes){
            if (scene != documentScene && canSendUIEvents(scene))
                if (button == D_MOUSE_BUTTON_1)
                    if (scene->getSystem<UISystem>()->eventOnPointerDown(x, y))
                        uiEventReceived = true;
        }

        if (dispatchToWorldDocuments(scenes, uiEventReceived, false, [&](UIDocumentSystem* documents, bool covered){
                return documents->dispatchWorldPointer(0, button, x, y, mods, PointerPhase::DOWN, false, covered);
            }))
            uiEventReceived = true;

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
    if (!transformCoordPos(x, y)){
        // released outside the canvas: no event, but the button is not held any more
        Input::releaseMousePressed(button);
        if (Engine::isCallTouchInMouseEvent())
            Input::removeTouch(0);
        return;
    }
    {
        Input::releaseMousePressed(button);
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

        if (dispatchReleaseToDocuments(scenes, 0, button, false, [&](UIDocumentSystem* documents){
                return documents->eventOnPointerUp(0, button, x, y, mods, false, false);
            }))
            uiEventReceived = true;

        for (Scene* scene : scenes){
            // a covered scene still gets the release, like the component UI
            if (scene->canReceiveUIEvents())
                if (button == D_MOUSE_BUTTON_1)
                    if (scene->getSystem<UISystem>()->eventOnPointerUp(x, y))
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
    // the movement counts outside the canvas too (a captured mouse looks around)
    const bool insideCanvas = transformCoordPos(x, y);
    Input::addMouseMovement(x, y);
    if (insideCanvas){
        Input::setMousePosition(x, y);
        if (mods != 0)
            Input::setModifiers(mods);

        uiEventReceived = false;
        const std::vector<Scene*> scenes = getScenesSnapshot();

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
                if (ui->eventOnPointerMove(x, y))
                    uiEventReceived = true;
                if (ui->isPointerOverUI())
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

    const bool consumed = dispatchToDocuments(getScenesSnapshot(), [&](UIDocumentSystem* documents){
        return documents->eventOnWheel(xoffset, yoffset, mods);
    }) != nullptr;

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onMouseScroll.call(xoffset, yoffset, mods);
}

void Engine::systemMouseEnter(){
    Input::addMouseEntered();
    // the pointer comes back elsewhere: no movement in between
    Input::resetMouseMovement();
    // documents hover again with the next move
    Engine::onMouseEnter.call();
}

void Engine::systemMouseLeave(){
    Input::releaseMouseEntered();
    for (Scene* scene : getScenesSnapshot())
        scene->getSystem<UIDocumentSystem>()->eventOnMouseLeave();
    Engine::onMouseLeave.call();
}

void Engine::systemKeyDown(int key, bool repeat, int mods){
    Input::addKeyPressed(key, repeat);
    if (mods != 0)
        Input::setModifiers(mods);

    const std::vector<Scene*> scenes = getScenesSnapshot();

    // the focused document first: a text field takes the keys it types with
    const bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
        return documents->eventOnKeyDown(key, repeat, mods);
    }) != nullptr;

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onKeyDown.call(key, repeat, mods);

    if (!consumed){
        for (Scene* scene : scenes){
            if (canSendUIEvents(scene))
                scene->getSystem<UISystem>()->eventOnKeyDown(key, repeat, mods);
        }
    }
}

void Engine::systemKeyUp(int key, bool repeat, int mods){
    Input::releaseKeyPressed(key);
    Input::setModifiers(mods); // Now it can be 0

    // every document hears the release; the one that took the press consumes it
    bool consumed = false;
    for (Scene* scene : getScenesSnapshot()){
        if (scene->getSystem<UIDocumentSystem>()->eventOnKeyUp(key, mods))
            consumed = true;
    }

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onKeyUp.call(key, repeat, mods);
}

void Engine::systemCharInput(wchar_t codepoint){
    const std::vector<Scene*> scenes = getScenesSnapshot();

    std::vector<uint32_t> codepoints;
    charInputCombiner.push((uint32_t)codepoint, codepoints);
    // Input.inputString
    for (uint32_t typed : codepoints)
        Input::addInputCharacter(typed);

    bool consumed;
    if (!codepoints.empty()){
        const std::string utf8 = toUtf8(codepoints);
        consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnTextInput(utf8);
        }) != nullptr;
    }else{
        // half of a pair: it goes where its other half will
        consumed = wantsTextComposition();
    }

    if (!ignoreEventsHandledByUI || !consumed)
        onCharInput.call(codepoint);

    if (!consumed){
        for (Scene* scene : scenes){
            if (canSendUIEvents(scene))
                scene->getSystem<UISystem>()->eventOnCharInput(codepoint);
        }
    }
}

void Engine::flushCharInput(){
    std::vector<uint32_t> codepoints;
    charInputCombiner.flush(codepoints);
    if (codepoints.empty())
        return;

    // a high surrogate whose low one never came
    const std::string utf8 = toUtf8(codepoints);
    dispatchToDocuments(getScenesSnapshot(), [&](UIDocumentSystem* documents){
        return documents->eventOnTextInput(utf8);
    });
}

void Engine::systemTextInput(const std::string& utf8){
    if (utf8.empty())
        return;

    const std::vector<Scene*> scenes = getScenesSnapshot();

    bool hadInvalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(utf8, hadInvalid);
    // Input.inputString
    for (uint32_t codepoint : codepoints)
        Input::addInputCharacter(codepoint);

    const bool consumed = dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
        return documents->eventOnTextInput(utf8);
    }) != nullptr;

    if (ignoreEventsHandledByUI && consumed)
        return;

    for (uint32_t codepoint : codepoints)
        sendCharacterToGame(codepoint, !consumed, true, scenes);
}

void Engine::systemImeComposition(const std::string& utf8, int cursorCodepoints){
    dispatchToDocuments(getScenesSnapshot(), [&](UIDocumentSystem* documents){
        return documents->eventOnImeComposition(utf8, cursorCodepoints);
    });
}

void Engine::systemImeCancel(){
    for (Scene* scene : getScenesSnapshot())
        scene->getSystem<UIDocumentSystem>()->eventOnImeCancel();
}

bool Engine::wantsTextComposition(){
    for (Scene* scene : getScenesSnapshot()){
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

    Input::removeGamepad(gamepad);
    Engine::onGamepadDisconnect.call(gamepad);
}

void Engine::systemGamepadButtonDown(int gamepad, int button){
    Input::addGamepadButtonPressed(gamepad, button);

    const bool consumed = gamepadKeyDown(getScenesSnapshot(), gamepad, gamepadKeyOfButton(button));

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onGamepadButtonDown.call(gamepad, button);
}

void Engine::systemGamepadButtonUp(int gamepad, int button){
    Input::releaseGamepadButtonPressed(gamepad, button);

    // consumed iff its down was, so the game never gets an up without a down
    const bool consumed = gamepadKeyUp(getScenesSnapshot(), gamepad, gamepadKeyOfButton(button));

    if (!ignoreEventsHandledByUI || !consumed)
        Engine::onGamepadButtonUp.call(gamepad, button);
}

void Engine::systemGamepadAxisMove(int gamepad, int axis, float value){
    Input::setGamepadAxisValue(gamepad, axis, value);

    const bool consumed = gamepadAxisToDocuments(getScenesSnapshot(), gamepad, axis, value);

    const std::pair<int, int> id(gamepad, axis);
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

bool Engine::wantsGamepadNavigation(){
    return documentsHaveGamepadFocus(getScenesSnapshot());
}

bool Engine::isTextInputFocused(){
    for (Scene* scene : getScenesSnapshot()){
        if (canSendUIEvents(scene) && scene->getSystem<UISystem>()->isInputFieldFocused())
            return true;
        if (canSendDocumentEvents(scene) && scene->getSystem<UIDocumentSystem>()->hasFocusedTextInput())
            return true;
    }
    return false;
}

void Engine::updateGamepadNavigation(){
    if (gamepadUI.held.empty())
        return;

    const double now = HtmlUIClock::now();
    std::vector<Scene*> scenes;
    // by index: a listener may press or release keys (send_input, Lua)
    for (size_t i = 0; i < gamepadUI.held.size(); i++){
        GamepadUIHeld& held = gamepadUI.held[i];
        if (!isGamepadDirectionKey(held.key) || !held.consumed || now < held.nextRepeat)
            continue;
        held.nextRepeat += gamepadRepeatInterval;
        if (held.nextRepeat <= now)
            held.nextRepeat = now + gamepadRepeatInterval;
        const int gamepad = held.gamepad;
        const GamepadUIKey key = held.key;
        if (scenes.empty())
            scenes = getScenesSnapshot();
        dispatchToDocuments(scenes, [&](UIDocumentSystem* documents){
            return documents->eventOnGamepadKeyDown(gamepad, key, true);
        });
    }
}
