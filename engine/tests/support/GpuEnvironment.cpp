// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GpuEnvironment.h"
#include "render/SystemRender.h"
#include "Engine.h"
#include "Scene.h"
#include "subsystem/RenderSystem.h"

#include "HeadlessContext.h"
#include "TestSystem.h"

#include "pool/ShaderPool.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    bool engineInitialised = false;
    bool viewLoaded = false;
    bool failed = false;

    TestSystem* testSystem = nullptr;
    Framebuffer* framebuffer = nullptr;

    const int defaultWidth = 640;
    const int defaultHeight = 360;
}

void GpuEnvironment::initEngine(int argc, char** argv){
    if (engineInitialised)
        return;

    // owned by the engine for the whole process, like the platform backends' System
    testSystem = new TestSystem();
    testSystem->setScreenSize(defaultWidth, defaultHeight);
    Engine::systemInit(argc, argv, testSystem);
    Engine::setCanvasSize(defaultWidth, defaultHeight);
    Engine::setScalingMode(Scaling::FITWIDTH);

    engineInitialised = true;
}

bool GpuEnvironment::ensure(){
    if (viewLoaded)
        return true;
    if (failed || !engineInitialised)
        return false;

#if defined(JASPER_TEST_HAS_GPU)
    if (!HeadlessContext::create(defaultWidth, defaultHeight)){
        failed = true;
        return false;
    }

    // every scene draws into this target instead of a swapchain
    framebuffer = new Framebuffer();
    framebuffer->setWidth(testSystem->getScreenWidth());
    framebuffer->setHeight(testSystem->getScreenHeight());
    Engine::setFramebuffer(framebuffer);

    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    // shaders compile on the calling thread, so a frame waits for them instead of skipping draws
    ShaderPool::setAsyncCompilation(false);

    viewLoaded = true;
    std::printf("[tests] headless %s device ready\n", HeadlessContext::getBackendName());
    return true;
#else
    std::fprintf(stderr, "GpuEnvironment: %s\n", unavailableReason());
    failed = true;
    return false;
#endif
}

bool GpuEnvironment::isReady(){
    return viewLoaded;
}

const char* GpuEnvironment::unavailableReason(){
#if defined(JASPER_TEST_HAS_GPU)
    if (!engineInitialised)
        return "the engine is not initialised (GpuEnvironment::initEngine)";
#if defined(__APPLE__)
    return "no headless GPU context (no Metal device)";
#else
    return "no headless GPU context (needs an X display: run under xvfb-run)";
#endif
#else
    return "this test binary has no headless GPU context: GPU suites run in the standalone engine tree on Linux and macOS";
#endif
}

void GpuEnvironment::setView(int screenWidth, int screenHeight, int canvasWidth, int canvasHeight, Scaling scaling){
    testSystem->setScreenSize(screenWidth, screenHeight);
    Engine::setCanvasSize(canvasWidth, canvasHeight);
    Engine::setScalingMode(scaling);
    // recomputes the canvas and view rect and resizes the target
    Engine::systemViewChanged();
}

namespace {
    // a mesh's detail levels still being built (Mesh LOD is on in a new Scene): a frame drawn now
    // shows its finest level, and a later one its own
    bool meshLodsPending(){
        for (Scene* scene : Engine::getSceneLayers()){
            std::shared_ptr<RenderSystem> render = scene ? scene->getSystem<RenderSystem>() : nullptr;
            if (render && render->hasPendingMeshLods())
                return true;
        }
        return false;
    }
}

bool GpuEnvironment::runFrames(std::function<bool()> ready, int maxFrames){
    // frames of a small scene are short: variants compiled and created off the render thread
    // (spec S32) get at least a minute whatever the frame count
    const auto start = std::chrono::steady_clock::now();
    const auto minimum = std::chrono::seconds(60);
    for (int i = 0; i < maxFrames || std::chrono::steady_clock::now() - start < minimum; i++){
        Engine::systemDraw();
        // and nothing still being created off the render thread (spec S32): what is pending
        // is not drawn yet
        if ((!ready || ready()) && SystemRender::isGpuCreationIdle() && !meshLodsPending()){
            drawFrames(2);
            return true;
        }
    }
    return false;
}

void GpuEnvironment::drawFrames(int count){
    for (int i = 0; i < count; i++){
        Engine::systemDraw();
    }
}

Framebuffer& GpuEnvironment::target(){
    return *framebuffer;
}

bool GpuEnvironment::isReferenceRenderer(){
#if defined(JASPER_TEST_HAS_GPU)
    const char* renderer = HeadlessContext::getRendererName();
    return !renderer || renderer[0] == '\0' || std::strstr(renderer, "llvmpipe") != nullptr;
#else
    return true;   // no device: nothing is compared
#endif
}

void GpuEnvironment::shutdown(){
    if (viewLoaded){
        Engine::systemViewDestroyed();
        viewLoaded = false;
    }
    if (engineInitialised){
        Engine::systemShutdown();
        Engine::setFramebuffer(nullptr);
    }
    delete framebuffer;
    framebuffer = nullptr;
#if defined(JASPER_TEST_HAS_GPU)
    HeadlessContext::destroy();
#endif
}

void GpuTestEnvironment::TearDown(){
    GpuEnvironment::shutdown();
}
