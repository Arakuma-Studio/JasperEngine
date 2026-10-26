// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorFrame.h"
#include "diag/HangWatchdog.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Log.h"

#include "imgui.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"

#include <chrono>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

// Seconds without activity before the loop starts idling
constexpr double IDLE_ENTER_DELAY = 0.5;

double frameMonotonicSeconds(){
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
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

    const bool activity =
        io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
        io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
        ImGui::IsAnyMouseDown() || typing || io.WantTextInput ||
        ImGui::IsAnyItemActive() || app->didRenderScene() ||
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
    // running (paused or loading) so a game-held cursor lock can't trap the mouse.
    Backend::setMouseControlSuspended(
        playSessionActive && !project->isMainScenePlaying());

    const double frameStart = frameMonotonicSeconds();
    const bool idleFrame = !state.forceRedraw && isIdle();

    // Unfocused always drops synchronization, so a blocked present can never
    // stall the event loop that serves clipboard requests.
    const bool vsync = app->isBenchmarkMode() ? false : (playSessionActive
        ? project->isVSyncEnabled()
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

    const bool renderRequested = state.forceRedraw || playSessionActive ||
                                 !idleFrame || state.redrawRequested ||
                                 app->hasPendingMainThreadTasks() || frameRequest;
    const bool frameReady = !state.minimized && renderRequested && renderer->beginFrame();

    // a profiler frame is one rendered loop of the editor (S15 §6.5); idle loops are not frames
    if (frameReady){
        FrameTimes::beginFrame();
        JE_PROFILE_FRAME();
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

    // a benchmark measures the frames as fast as they go, focused or not (a headless X server
    // gives no window the focus)
    if ((!state.focused || state.minimized) && !app->isBenchmarkMode()){
        const int sleepMs = static_cast<int>(
            (state.framePeriod - (frameMonotonicSeconds() - frameStart)) * 1000.0);
        if (sleepMs > 0){
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    return true;
}
