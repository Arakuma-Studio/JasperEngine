// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "GameWindow.h"

#include "imgui_internal.h"
#include "Engine.h"
#include "Backend.h"
#include "Theme.h"
#include "subsystem/UIDocumentSystem.h"
#include "Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/UIDocumentSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace jasperengine;

namespace {

    struct AspectPreset {
        const char* label;
        float width;
        float height;
    };

    // Index 1 is the project's canvas resolution, filled in at runtime
    const AspectPreset ASPECT_PRESETS[] = {
        { "Free Aspect", 0.0f, 0.0f },
        { nullptr, 0.0f, 0.0f },
        { "16:9", 16.0f, 9.0f },
        { "16:10", 16.0f, 10.0f },
        { "4:3", 4.0f, 3.0f },
        { "9:16 Portrait", 9.0f, 16.0f },
    };
    constexpr int ASPECT_COUNT = IM_ARRAYSIZE(ASPECT_PRESETS);
    constexpr int ASPECT_PROJECT = 1;

    constexpr float MIN_SCALE = 1.0f;
    constexpr float MAX_SCALE = 5.0f;

    bool isRunning(editor::ScenePlayState state) {
        return state == editor::ScenePlayState::PLAYING || state == editor::ScenePlayState::PAUSED;
    }

    // Unity-style toggle: flat button, highlighted while on
    bool toggleButton(const char* label, bool on, const char* tooltip) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, editor::Theme::Colors::ButtonActivated);
        const bool pressed = ImGui::Button(label);
        if (on) ImGui::PopStyleColor();
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    }

}

editor::GameWindow::GameWindow(Project* project, PlayInput* playInput)
    : project(project), playInput(playInput) {
}

// Settings load after the windows are built, so read them on first use
void editor::GameWindow::loadSettings() {
    if (settingsLoaded) return;
    settingsLoaded = true;
    settings = AppSettings::getGameViewSettings();
    settings.aspect = std::clamp(settings.aspect, 0, ASPECT_COUNT - 1);
    settings.scale = std::clamp(settings.scale, MIN_SCALE, MAX_SCALE);
}

void editor::GameWindow::saveSettings() {
    if (settings != AppSettings::getGameViewSettings()) {
        AppSettings::setGameViewSettings(settings);
    }
}

void editor::GameWindow::setOpen(bool open) {
    windowOpen = open;
    if (!open) {
        visible = false;
        focused = false;
        cursorInView = false;
    }
}

bool editor::GameWindow::isOpen() const {
    return windowOpen;
}

void editor::GameWindow::requestFocus() {
    windowOpen = true;
    focusRequested = true;
}

uint32_t editor::GameWindow::getTargetSceneId() const {
    const uint32_t playSceneId = project->getPlaySessionSceneId();
    if (playSceneId != NULL_PROJECT_SCENE) {
        return playSceneId;
    }
    return project->getSelectedSceneId();
}

bool editor::GameWindow::hostsScene(uint32_t sceneId) const {
    if (sceneId == NULL_PROJECT_SCENE || sceneId != project->getPlaySessionSceneId()) {
        return false;
    }
    const SceneProject* sceneProject = project->getScene(sceneId);
    return sceneProject && isRunning(sceneProject->playState);
}

bool editor::GameWindow::wantsEditPass(uint32_t sceneId) const {
    if (!visible || width <= 0 || height <= 0) {
        return false;
    }
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE || sceneId != project->getSelectedSceneId()) {
        return false;
    }
    const SceneProject* sceneProject = project->getScene(sceneId);
    return sceneProject && sceneProject->playState == ScenePlayState::STOPPED
        && project->getGameCamera(sceneId) != NULL_ENTITY;
}

bool editor::GameWindow::isEditPassDirty() const {
    return editPassDirty || !editPassRendered || editPassSceneId != project->getSelectedSceneId();
}

void editor::GameWindow::markEditPassRendered(uint32_t sceneId) {
    editPassSceneId = sceneId;
    editPassRendered = true;
    editPassDirty = false;
}

Framebuffer* editor::GameWindow::getFramebuffer() {
    return &framebuffer;
}

bool editor::GameWindow::isVisible() const {
    return visible;
}

int editor::GameWindow::getWidth() const {
    return width;
}

int editor::GameWindow::getHeight() const {
    return height;
}

int editor::GameWindow::getRenderWidth() const {
    if (width > 0 && height > 0) {
        return width;
    }
    return std::max(1, (int)project->getCanvasWidth());
}

int editor::GameWindow::getRenderHeight() const {
    if (width > 0 && height > 0) {
        return height;
    }
    return std::max(1, (int)project->getCanvasHeight());
}

bool editor::GameWindow::isGameCursorInView() const {
    return cursorInView;
}

bool editor::GameWindow::hasKeyboardFocus() const {
    return focused;
}

void editor::GameWindow::resetProjectState() {
    playInput->releaseAll();
    visible = false;
    focused = false;
    cursorInView = false;
    editPassSceneId = NULL_PROJECT_SCENE;
    editPassRendered = false;
    editPassDirty = true;
    if (framebuffer.isCreated()) {
        framebuffer.destroy();
    }
}

const editor::GameViewSettings& editor::GameWindow::getSettings() {
    loadSettings();
    return settings;
}

void editor::GameWindow::applySettings(const GameViewSettings& newSettings) {
    loadSettings();
    settings = newSettings;
    settings.aspect = std::clamp(settings.aspect, 0, ASPECT_COUNT - 1);
    settings.scale = std::clamp(settings.scale, MIN_SCALE, MAX_SCALE);
    editPassDirty = true;
    saveSettings();
}

int editor::GameWindow::getAspectCount() {
    return ASPECT_COUNT;
}

bool editor::GameWindow::computeAspect(float& aspect) const {
    if (settings.aspect == ASPECT_PROJECT) {
        const float w = (float)project->getCanvasWidth();
        const float h = (float)project->getCanvasHeight();
        if (w > 0.0f && h > 0.0f) {
            aspect = w / h;
            return true;
        }
        return false;
    }
    const AspectPreset& preset = ASPECT_PRESETS[settings.aspect];
    if (preset.width > 0.0f && preset.height > 0.0f) {
        aspect = preset.width / preset.height;
        return true;
    }
    return false;
}

void editor::GameWindow::show() {
    loadSettings();

    const bool wasVisible = visible;
    visible = false;
    focused = false;
    cursorInView = false;

    const uint32_t sceneId = getTargetSceneId();
    const bool sessionActive = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;

    if (!windowOpen) {
        if (wasVisible) playInput->release(sceneId);
        maximizedShown = false;
        hostedLastFrame = false;
        return;
    }

    // First launch without a saved dock slot: join the scene tabs, as Unity does.
    // Scene windows are drawn after this one, so wait until they exist.
    ImGuiWindow* dockedNextToScene = nullptr;
    if (!ImGui::FindWindowByName(WINDOW_NAME) && !ImGui::FindWindowSettingsByID(ImHashStr(WINDOW_NAME))) {
        ImGuiWindow* sceneWindow = ImGui::FindWindowByName(("###Scene" + std::to_string(project->getSelectedSceneId())).c_str());
        if (sceneWindow && sceneWindow->DockId != 0) {
            ImGui::SetNextWindowDockID(sceneWindow->DockId, ImGuiCond_FirstUseEver);
            dockedNextToScene = sceneWindow;
        } else if (++dockWaitFrames < 10) {
            return;
        }
    }

    const bool maximize = settings.maximizeOnPlay && sessionActive;

    if (focusRequested && !maximize) {
        ImGui::SetNextWindowFocus();
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(200, 150), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin(WINDOW_NAME, &windowOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (maximize) {
            ImGui::TextDisabled("Maximized while playing");
        } else {
            drawContent(sceneId);
        }
    }
    ImGui::End();

    // Joining the tab bar selects the new tab; keep the Scene view in front
    if (dockedNextToScene && !focusRequested) {
        ImGui::FocusWindow(dockedNextToScene);
    }

    if (maximize && windowOpen) {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        if (!maximizedShown || focusRequested) {
            ImGui::SetNextWindowFocus();
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::Begin("Game##Maximized", nullptr, flags)) {
            drawContent(sceneId);
        }
        ImGui::End();
        maximizedShown = true;
    } else {
        maximizedShown = false;
    }

    focusRequested = false;

    if (!visible) {
        hostedLastFrame = false;
        if (wasVisible) playInput->release(sceneId);
    } else if (!wasVisible) {
        // The scene may have changed while this view was hidden
        editPassDirty = true;
    }
}

void editor::GameWindow::drawToolbar() {
    float toolbarRowY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(toolbarRowY - (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Display 1").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (ImGui::BeginCombo("##GameDisplay", "Display 1")) {
        ImGui::Selectable("Display 1", true);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Target display");

    const std::string projectAspect = std::to_string(project->getCanvasWidth()) + "x"
        + std::to_string(project->getCanvasHeight()) + " (Project)";
    auto aspectLabel = [&](int index) -> const char* {
        return index == ASPECT_PROJECT ? projectAspect.c_str() : ASPECT_PRESETS[index].label;
    };

    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    if (ImGui::BeginCombo("##GameAspect", aspectLabel(settings.aspect))) {
        for (int i = 0; i < ASPECT_COUNT; i++) {
            if (ImGui::Selectable(aspectLabel(i), settings.aspect == i)) {
                settings.aspect = i;
                editPassDirty = true;
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Aspect ratio of the game view");

    ImGui::SameLine(0, Theme::dpi(12.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderFloat("##GameScale", &settings.scale, MIN_SCALE, MAX_SCALE, "%.1fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        saveSettings();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        settings.scale = 1.0f;
        saveSettings();
    }
    ImGui::SetItemTooltip("Zoom into the game image (double-click resets)");

    // The played scene's HTML UI documents (the RmlUi debugger inspects and draws in them)
    UIDocumentSystem* playedDocuments = nullptr;
    const uint32_t playedSceneId = project->getPlaySessionSceneId();
    if (playedSceneId != NULL_PROJECT_SCENE) {
        SceneProject* played = project->getScene(playedSceneId);
        if (played && played->scene && played->playState == ScenePlayState::PLAYING) {
            playedDocuments = played->scene->getSystem<UIDocumentSystem>().get();
        }
    }
    if (!playedDocuments && htmlUiDebuggerSceneId != NULL_PROJECT_SCENE) {
        // play stopped: the debugger was the game's
        SceneProject* shown = project->getScene(htmlUiDebuggerSceneId);
        if (shown && shown->scene) {
            if (UIDocumentSystem* documents = shown->scene->getSystem<UIDocumentSystem>().get()) documents->setDebuggerVisible(false);
        }
        htmlUiDebuggerSceneId = NULL_PROJECT_SCENE;
    }

    // Right side toggles, as icons when the view is too narrow for their names
    const char* debuggerLabel = "HTML UI Debugger##GameHtmlUiDebugger";
    const char* touchLabel = ICON_FA_MOBILE_SCREEN "##GameTouch";
    const char* maximizeLabel = "Maximize On Play##GameMaximize";
    const char* statsLabel = "Stats##GameStats";
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    };
    auto groupWidth = [&]() {
        return buttonWidth(debuggerLabel) + buttonWidth(touchLabel) + buttonWidth(maximizeLabel) + buttonWidth(statsLabel) +
               style.ItemSpacing.x * 3.0f;
    };
    ImGui::SameLine();
    const float contentRight = ImGui::GetWindowContentRegionMax().x;
    if (contentRight - groupWidth() < ImGui::GetCursorPosX()) {
        debuggerLabel = ICON_FA_BUG "##GameHtmlUiDebugger";
        maximizeLabel = ICON_FA_MAXIMIZE "##GameMaximize";
        statsLabel = ICON_FA_CHART_SIMPLE "##GameStats";
    }
    const float rightX = contentRight - groupWidth();
    if (rightX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(rightX);
    }

    const bool debuggerShown = playedDocuments && playedDocuments->isDebuggerVisible();
    ImGui::BeginDisabled(!playedDocuments);
    if (toggleButton(debuggerLabel, debuggerShown, "HTML UI Debugger: RmlUi's element inspector over the playing scene's documents")) {
        if (playedDocuments->setDebuggerVisible(!debuggerShown)) {
            htmlUiDebuggerSceneId = debuggerShown ? NULL_PROJECT_SCENE : playedSceneId;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool simulateTouch = project->isSimulateTouch();
    ImGui::BeginDisabled(project->isAnyScenePlaying());
    if (toggleButton(touchLabel, simulateTouch, "Simulate touch: play as a touch device (the mouse acts as one finger)")) {
        project->setSimulateTouch(!simulateTouch);
        project->saveWorkspaceFile();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (toggleButton(maximizeLabel, settings.maximizeOnPlay, "Maximize On Play: fill the editor with the Game view while playing")) {
        settings.maximizeOnPlay = !settings.maximizeOnPlay;
        saveSettings();
    }

    ImGui::SameLine();
    if (toggleButton(statsLabel, settings.showStats, "Stats: show rendering statistics")) {
        settings.showStats = !settings.showStats;
        saveSettings();
    }

    ImGui::SetCursorPosY(toolbarRowY + ImGui::GetTextLineHeightWithSpacing());
}

void editor::GameWindow::drawContent(uint32_t sceneId) {
    drawToolbar();

    const bool focusCanvas = focusRequested;
    ImGui::BeginChild("GameCanvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (focusCanvas) {
            ImGui::SetWindowFocus();
        }

        visible = true;
        focused = ImGui::IsWindowFocused();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasMax(canvasMin.x + avail.x, canvasMin.y + avail.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));

        // Game area: the whole view, or the largest rect of the chosen aspect
        ImVec2 areaSize = avail;
        float aspect = 0.0f;
        if (computeAspect(aspect) && avail.x > 0.0f && avail.y > 0.0f) {
            if (avail.x / avail.y > aspect) {
                areaSize = ImVec2(std::floor(avail.y * aspect), avail.y);
            } else {
                areaSize = ImVec2(avail.x, std::floor(avail.x / aspect));
            }
        }
        const ImVec2 areaMin(std::floor(canvasMin.x + (avail.x - areaSize.x) * 0.5f),
                             std::floor(canvasMin.y + (avail.y - areaSize.y) * 0.5f));

        // Engine pixels per ImGui pixel, as the Scene view computes it
        ImGuiViewport* canvasViewport = ImGui::GetWindowViewport();
        ImVec2 fbScale = (canvasViewport && canvasViewport->FramebufferScale.x > 0.0f && canvasViewport->FramebufferScale.y > 0.0f)
            ? canvasViewport->FramebufferScale
            : ImGui::GetIO().DisplayFramebufferScale;
        float dpiScale = (canvasViewport && canvasViewport->DpiScale > 0.0f) ? canvasViewport->DpiScale : 1.0f;
        if (dpiScale < 0.5f || dpiScale > 8.0f) dpiScale = 1.0f;
        engineScale = Backend::sceneRenderScale(fbScale, dpiScale);

        const int newWidth = areaSize.x > 0.0f ? std::max(1, (int)std::lround(areaSize.x * engineScale.x)) : 0;
        const int newHeight = areaSize.y > 0.0f ? std::max(1, (int)std::lround(areaSize.y * engineScale.y)) : 0;
        bool resized = false;
        if (newWidth != width || newHeight != height) {
            width = newWidth;
            height = newHeight;
            editPassDirty = true;
            resized = true;
        }

        // Scale zooms into the image around its centre; the child clips the rest
        const float zoom = std::clamp(settings.scale, MIN_SCALE, MAX_SCALE);
        const ImVec2 imageSize(areaSize.x * zoom, areaSize.y * zoom);
        const ImVec2 imageMin(areaMin.x + (areaSize.x - imageSize.x) * 0.5f,
                              areaMin.y + (areaSize.y - imageSize.y) * 0.5f);
        const ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);

        SceneProject* sceneProject = project->getScene(sceneId);
        const bool running = sceneProject && isRunning(sceneProject->playState)
            && sceneId == project->getPlaySessionSceneId();

        if (running) {
            // A paused game only redraws on demand: resizing or coming back into view
            // has to redraw it at this size
            if (resized || !hostedLastFrame) {
                sceneProject->needUpdateRender = true;
            }
            // Play changes the scene: redraw the edit view once it stops
            editPassDirty = true;
        }
        hostedLastFrame = running;

        ImTextureID texture{};
        const char* message = nullptr;
        if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender) {
            message = "No scene open";
        } else if (running) {
            if (framebuffer.isCreated()) {
                texture = Backend::getImGuiTexture(&framebuffer.getRender().getColorTexture());
            }
        } else if (sceneProject->playState == ScenePlayState::LOADING || sceneProject->playState == ScenePlayState::SAVING) {
            message = "Starting...";
        } else if (sceneProject->playState == ScenePlayState::CANCELLING) {
            message = "Stopping...";
        } else if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
            message = "Starting...";
        } else if (project->getGameCamera(sceneId) == NULL_ENTITY) {
            message = "No cameras rendering";
        } else if (editPassRendered && editPassSceneId == sceneId && framebuffer.isCreated()) {
            texture = Backend::getImGuiTexture(&framebuffer.getRender().getColorTexture());
        }

        if (texture != ImTextureID{}) {
            drawList->PushClipRect(canvasMin, canvasMax, true);
            drawList->AddImage(texture, imageMin, imageMax);
            drawList->PopClipRect();
        }

        if (message) {
            const ImVec2 textSize = ImGui::CalcTextSize(message);
            drawList->AddText(ImVec2(canvasMin.x + (avail.x - textSize.x) * 0.5f, canvasMin.y + (avail.y - textSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
        }

        const bool hovered = ImGui::IsWindowHovered();

        if (running && sceneProject->playState == ScenePlayState::PLAYING) {
            const ImVec2 inputScale(engineScale.x / zoom, engineScale.y / zoom);
            playInput->forward(sceneId, imageMin, inputScale, hovered, focused);
            cursorInView = hovered;
        } else {
            playInput->release(sceneId);
        }

        if (settings.showStats) {
            drawStats(areaMin, ImVec2(areaMin.x + areaSize.x, areaMin.y + areaSize.y), sceneProject);
        }
    }
    ImGui::EndChild();
}

void editor::GameWindow::drawStats(ImVec2 imageMin, ImVec2 imageMax, const SceneProject* sceneProject) {
    const Engine::FrameStats& frameStats = Engine::getFrameStats();
    const float fps = ImGui::GetIO().Framerate;
    const float ms = fps > 0.0f ? 1000.0f / fps : 0.0f;

    char lines[5][96];
    int lineCount = 4;
    snprintf(lines[0], sizeof(lines[0]), "Statistics");
    snprintf(lines[1], sizeof(lines[1]), "Graphics: %.1f FPS (%.1f ms)", fps, ms);
    snprintf(lines[2], sizeof(lines[2]), "Draw calls: %u   Tris: %llu", frameStats.drawCalls, (unsigned long long)frameStats.triangles);
    snprintf(lines[3], sizeof(lines[3]), "Screen: %dx%d", width, height);
    // the scene's HTML UI documents (their offscreen passes are not in the counters above)
    UIDocumentSystem* documents = (sceneProject && sceneProject->scene) ? sceneProject->scene->getSystem<UIDocumentSystem>().get() : nullptr;
    if (documents && documents->hasDocuments()) {
        const HtmlUIRenderStats& ui = documents->getRenderStats();
        snprintf(lines[4], sizeof(lines[4]), "HTML UI: %u passes, %u draws, %u pipelines", ui.passes, ui.drawCalls, ui.pipelineApplies);
        lineCount = 5;
    }

    float boxWidth = 0.0f;
    for (int i = 0; i < lineCount; i++) boxWidth = std::max(boxWidth, ImGui::CalcTextSize(lines[i]).x);
    const ImVec2 pad = Theme::dpi(ImVec2(10.0f, 8.0f));
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 boxSize(boxWidth + pad.x * 2.0f, lineHeight * (float)lineCount + pad.y * 2.0f - ImGui::GetStyle().ItemSpacing.y);

    const float margin = Theme::dpi(8.0f);
    ImVec2 boxMin(imageMax.x - boxSize.x - margin, imageMin.y + margin);
    boxMin.x = std::max(boxMin.x, imageMin.x + margin);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), IM_COL32(20, 20, 20, 210), Theme::dpi(3.0f));
    for (int i = 0; i < lineCount; i++) {
        const ImU32 color = i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255);
        drawList->AddText(ImVec2(boxMin.x + pad.x, boxMin.y + pad.y + lineHeight * i), color, lines[i]);
    }
}
