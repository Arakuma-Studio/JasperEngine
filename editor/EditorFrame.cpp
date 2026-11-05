// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorFrame.h"
#include "render/SystemRender.h"
#include "shader/ShaderWarmup.h"
#include "diag/HangWatchdog.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Engine.h"
#include "Log.h"
#include "Platform.h"

#include "window/EditorModal.h"

#include "imgui.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

// Seconds without activity before the loop starts idling
constexpr double IDLE_ENTER_DELAY = 0.5;

double frameMonotonicSeconds(){
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// On Windows sleep_for wakes on the 15.6 ms system tick, too coarse to pace frames
void sleepSeconds(double seconds){
#if defined(_WIN32)
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7); // relative, in 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)){
        WaitForSingleObject(timer, INFINITE);
        return;
    }
#endif
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}

void EditorFrame::init(Renderer& renderer, App& app, void (*platformNewFrame)()){
    this->renderer = &renderer;
    this->app = &app;
    this->platformNewFrame = platformNewFrame;
    lastActivityTime = frameMonotonicSeconds();
}

std::string EditorFrame::formatWindowTitle(const std::string& projectName){
    return projectName.empty()
        ? "Empty project - JasperEngine"
        : projectName + " - JasperEngine";
}

bool EditorFrame::isIdle() const{
    if (app && (app->isBenchmarkMode() || app->wantsContinuousFrames()))
        return false;
    return frameMonotonicSeconds() - lastActivityTime > IDLE_ENTER_DELAY;
}

// Input, an active widget, a scene redraw or queued cross-thread work keeps the
// loop at full rate; its absence lets it idle next frame.
bool EditorFrame::detectActivity(bool& redrawRequested){
    ImGuiIO& io = ImGui::GetIO();

    // The code editor reads keys itself, with no io.WantTextInput or active item
    // to check. Keyboard range only: a resting stick would never let it idle.
    bool typing = io.InputQueueCharacters.Size > 0;
    for (int key = ImGuiKey_NamedKey_BEGIN; !typing && key < ImGuiKey_GamepadStart; key++){
        typing = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
    }

    // Consumed apart from the test below, which short-circuits
    const bool appRedraw = app->consumeRedrawRequest();

    // A text field stays active while it has the caret, which blinks at the idle rate
    const bool activeWidget = ImGui::IsAnyItemActive() && !io.WantTextInput;

    const bool activity =
        io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
        io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
        ImGui::IsAnyMouseDown() || typing ||
        activeWidget || app->didRenderScene() ||
        app->hasPendingMainThreadTasks() || redrawRequested || appRedraw;
    redrawRequested = false;

    return activity;
}

bool EditorFrame::run(EditorFrameState& state){
    // the editor's heartbeat: every loop iteration and Windows live-resize frame comes here
    diag::heartbeat();
    Project* project = app->getProject();
    const bool playSessionActive = project->isPlaySessionActive();

    // Hand the cursor back to the editor while a play session isn't actively
    // running (paused or loading), the Game view does not have the focus, or the user
    // released it (Escape), so a game-held cursor lock can't trap the mouse.
    Backend::setMouseControlSuspended(
        playSessionActive && (!project->isMainScenePlaying() || app->getPlayInput()->wantsEditorCursor()));

    const double frameStart = frameMonotonicSeconds();
    const bool idleFrame = !state.forceRedraw && isIdle();

    // the window's focus reaches the game as Engine::onFocus (Unity's OnApplicationFocus)
    Platform::forwardHostFocus(state.focused, playSessionActive);

    // Unfocused always drops synchronization, so a blocked present can never
    // stall the event loop that serves clipboard requests. While playing, the game's
    // Engine.vSyncCount decides (it starts from the project's VSync setting).
    const FramePacing& pacing = Engine::getFramePacing();
    const bool vsync = (app->isBenchmarkMode() || app->isRecording()) ? false : (playSessionActive
        ? pacing.swapInterval > 0
        : AppSettings::getEditorVSyncEnabled());
    const bool frameSync = state.focused && vsync;
    const bool targetReady =
        state.minimized || renderer->updateTarget(state.width, state.height, frameSync);

    // submits are dropped after a GPU reset, so the window would just stop
    // updating. Checked after updateTarget, which is where a loss often lands.
    if (renderer->isDeviceLost()){
        Log::error("Graphics device was reset, closing the editor without saving");
        return false;
    }
    if (!targetReady){
        return false;
    }

    // Consumed apart from the test below, which short-circuits
    const bool frameRequest = app->consumeFrameRequest();

    // Keeps the caret of a focused text field blinking
    const bool textFocused = ImGui::GetIO().WantTextInput;

    const bool renderRequested = state.forceRedraw || playSessionActive ||
                                 !idleFrame || state.redrawRequested ||
                                 app->hasPendingMainThreadTasks() || frameRequest ||
                                 textFocused || !SystemRender::isGpuCreationIdle() ||
                                 // Auto Save runs in a frame: an idle editor wakes when a backup is due
                                 project->isAutosaveDue();
    const bool frameReady = !state.minimized && renderRequested && renderer->beginFrame();

    // a profiler frame is one rendered loop of the editor (S15 §6.5); idle loops are not frames
    if (frameReady){
        FrameTimes::beginFrame();
        JE_PROFILE_FRAME();
        // one shader clock for every view the frame draws (each draws through Engine::systemDraw)
        Engine::latchShaderTime();
        // the shaders and pipelines created off the render thread that are ready (spec S32); the
        // loop keeps drawing while any is pending (renderRequested), so they show up unprompted;
        // and what Play recorded for the shader warm-up list
        Engine::updateGpuCreation();
    }

    renderer->newFrame();
    platformNewFrame();
    ImGui::NewFrame();

    if (frameReady){
        JE_PROFILE_SCOPE("Scene Views");
        app->engineRender();
    }else{
        // engineRender() is what normally drains main-thread tasks. Keep draining
        // them so AI work, logging and async load callbacks still complete.
        app->processMainThreadTasks();
    }

    {
        JE_PROFILE_SCOPE("Editor UI");
        app->show();
    }
    // the windows are still active: the top-most modal is what MCP sees until the next frame
    EditorModal::endFrame();
    if (EditorModal::wantsFrame()) app->requestRedraw();

    const bool activity = detectActivity(state.redrawRequested);
    if (activity){
        lastActivityTime = frameMonotonicSeconds();
    }

    bool frameSubmitted = false;
    {
        // the editor's own rendering: CPU time only (its renderer is not sokol's)
        JE_PROFILE_SCOPE("Editor Render");
        ImGui::Render();
        if (frameReady){
            frameSubmitted = renderer->endFrame(ImGui::GetDrawData(), state.width, state.height);
        }

        if (!frameReady || frameSubmitted){
            renderer->renderViewports(renderRequested || activity);
        }
    }

    if (frameReady){
        FrameTimes::endFrameWork();
    }
    if (frameSubmitted){
        JE_PROFILE_SCOPE("Present/Wait");
        renderer->present();
    }
    renderer->purgeTextures();
    // a frame that drew (the editor window, or only its views: headless, minimized)
    if (frameReady || renderRequested){
        app->recordFrameTime((frameMonotonicSeconds() - frameStart) * 1000.0);
    }

    // Engine.targetFrameRate without vsync (or a vSyncCount past 1): the game's frame limiter
    const bool playLimited = playSessionActive && !app->isBenchmarkMode() && !state.minimized && pacing.limiterFps > 0.0;
    if (playLimited){
        // an unfocused editor never runs past its unfocused rate
        const double limit = state.focused || state.framePeriod <= 0.0
            ? pacing.limiterFps : std::min(pacing.limiterFps, 1.0 / state.framePeriod);
        playLimiter.wait(limit);
        frameDeadline = frameMonotonicSeconds();
    }else{
        playLimiter.reset();
    }

    // Unfocused and minimized frames have no vsync to pace them. A benchmark measures
    // the frames as fast as they go, focused or not (a headless X server gives no window the
    // focus), unless the game limits them (Engine.targetFrameRate)
    if (!playLimited && (!state.focused || state.minimized) && !app->isBenchmarkMode()){
        // Paced by deadline, so a late wake-up is made up on the next frame
        const double now = frameMonotonicSeconds();
        frameDeadline += state.framePeriod;
        if (std::abs(frameDeadline - now) > state.framePeriod){
            frameDeadline = now;
        }
        if (frameDeadline > now){
            sleepSeconds(frameDeadline - now);
        }
    }

    return true;
}
