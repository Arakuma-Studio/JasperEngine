// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "GameWindow.h"

#include "imgui_internal.h"
#include "Engine.h"
#include "Platform.h"
#include "Screen.h"
#include "util/PlatformRules.h"
#include "window/DeviceProfiles.h"
#include "Backend.h"
#include "Theme.h"
#include "subsystem/UIDocumentSystem.h"
#include "registry/PhysicsLayers.h"
#include "Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "net/Http.h"
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
    applySimulation();
}

void editor::GameWindow::applySimulation() {
    // the Custom device is the toolbar's own screen
    setCustomDevice(settings.custom);
    const DeviceProfile* device = settings.device.empty() ? nullptr : findDeviceProfile(settings.device);
    if (!device) settings.device.clear();
    ScreenOrientation orientation = PlatformRules::isOrientation(settings.orientation)
        ? (ScreenOrientation)settings.orientation : ScreenOrientation::Portrait;
    if (!PlatformRules::isFixedOrientation(orientation) ||
        (device && !(profileOrientationMask(*device) & PlatformRules::orientationBit(orientation)))) {
        orientation = ScreenOrientation::Portrait;
    }
    settings.orientation = (int)orientation;
    // a playing game keeps where its requests turned the device, unless the device changed
    const PlatformSimulation& current = Platform::getSimulation();
    const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    if (playing && device && current.device == device) {
        // the toolbar's orientation stays what Play stop goes back to
        const ScreenOrientation turned = current.orientation;
        Platform::setSimulatedDevice(device, orientation);
        Platform::setSimulatedOrientation(turned);
    } else {
        Platform::setSimulatedDevice(device, orientation);
    }
    Platform::setSimulatedLanguage(settings.language);
    editPassDirty = true;
}

ScreenOrientation editor::GameWindow::getLayoutOrientation() const {
    return layoutOrientation;
}

ScreenOrientation editor::GameWindow::renderOrientation() const {
    // laid out: the orientation of that size; never shown: the device's
    return (width > 0 && height > 0) ? layoutOrientation : Platform::getSimulation().orientation;
}

void editor::GameWindow::syncSafeArea() {
    const DeviceProfile* device = Platform::getSimulation().device;
    const ScreenOrientation orientation = renderOrientation();
    const int renderWidth = getRenderWidth();
    const int renderHeight = getRenderHeight();
    const bool custom = device && std::string(device->id) == "custom";

    const bool changed = device != safeAreaPushed.device || renderWidth != safeAreaPushed.width ||
                         renderHeight != safeAreaPushed.height || (device && orientation != safeAreaPushed.orientation) ||
                         (custom && settings.custom != safeAreaPushed.custom);
    if (changed) {
        safeAreaPushed.device = device;
        safeAreaPushed.orientation = orientation;
        safeAreaPushed.width = renderWidth;
        safeAreaPushed.height = renderHeight;
        safeAreaPushed.custom = settings.custom;
        if (!device) {
            // no device: no insets, as a desktop build
            Screen::setPlatformSafeArea(EdgeInsets());
        } else {
            // the device's pixels to the render's (the image has the device's aspect when shown)
            int deviceWidth = 0, deviceHeight = 0;
            profileSize(*device, orientation, deviceWidth, deviceHeight);
            const float sx = (float)renderWidth / (float)std::max(1, deviceWidth);
            const float sy = (float)renderHeight / (float)std::max(1, deviceHeight);
            const EdgeInsets insets = profileInsets(*device, orientation);
            const EdgeInsets scaled(std::round(insets.left * sx), std::round(insets.top * sy),
                                    std::round(insets.right * sx), std::round(insets.bottom * sy));
            std::vector<Rect> cutouts;
            const Rect cutout = profileCutout(*device, orientation);
            if (cutout.getWidth() > 0.0f && cutout.getHeight() > 0.0f) {
                cutouts.push_back(Rect(std::round(cutout.getX() * sx), std::round(cutout.getY() * sy),
                                       std::round(cutout.getWidth() * sx), std::round(cutout.getHeight() * sy)));
            }
            Screen::setPlatformSafeArea(scaled, cutouts);
        }
    }

    // edit mode: the editor scenes lay UI out for the project canvas, as a Play at this size would
    if (project->getPlaySessionSceneId() == NULL_PROJECT_SCENE) {
        Screen::setEditorReference((int)project->getCanvasWidth(), (int)project->getCanvasHeight(), renderWidth, renderHeight,
                                   project->getScalingMode());
        const uint64_t revision = Screen::getSafeAreaRevision();
        if (revision != editSafeAreaRevision) {
            editSafeAreaRevision = revision;
            // Safe Area panels move: the Scene views and the edit pass draw again
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                if (sceneProject) safeAreaRedrawPending.insert(sceneProject->id);
            }
            editPassDirty = true;
        }
    }
    // a Scene view draws only when shown: one behind another tab redraws when it comes forward
    if (!safeAreaRedrawPending.empty()) {
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (sceneProject && sceneProject->isVisible && safeAreaRedrawPending.erase(sceneProject->id)) {
                sceneProject->needUpdateRender = true;
            }
        }
    }
}

Rect editor::GameWindow::safeAreaInImage(float w, float h) const {
    const DeviceProfile* device = Platform::getSimulation().device;
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        // what the game sees: Screen.safeAreaPixels (extras included) over the rendered pixels
        const Rect pixels = Screen::getSafeAreaPixels();
        const float sx = w / (float)std::max(1, getRenderWidth());
        const float sy = h / (float)std::max(1, getRenderHeight());
        return Rect(pixels.getX() * sx, pixels.getY() * sy, pixels.getWidth() * sx, pixels.getHeight() * sy);
    }
    if (!device) return Rect(0, 0, w, h);
    const ScreenOrientation orientation = renderOrientation();
    int deviceWidth = 0, deviceHeight = 0;
    profileSize(*device, orientation, deviceWidth, deviceHeight);
    const Rect safe = profileSafeRect(*device, orientation);
    const float sx = w / (float)std::max(1, deviceWidth);
    const float sy = h / (float)std::max(1, deviceHeight);
    return Rect(safe.getX() * sx, safe.getY() * sy, safe.getWidth() * sx, safe.getHeight() * sy);
}

bool editor::GameWindow::getSafeAreaInImage(float w, float h, Rect& rect) const {
    if (!Platform::getSimulation().device) return false;
    rect = safeAreaInImage(w, h);
    return true;
}

uint8_t editor::GameWindow::getRotatableOrientations() const {
    const DeviceProfile* device = Platform::getSimulation().device;
    if (!device) return 0;
    uint8_t mask = profileOrientationMask(*device);
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        mask &= PlatformRules::effectiveMask(Platform::getRequestedOrientation(), Platform::getAllowedMask());
    }
    return mask;
}

bool editor::GameWindow::rotateDevice(std::string* error) {
    loadSettings();
    const DeviceProfile* device = Platform::getSimulation().device;
    if (!device) {
        if (error) *error = "no device is simulated (pick one in the Game view's Device menu)";
        return false;
    }
    const ScreenOrientation current = Platform::getSimulation().orientation;
    const ScreenOrientation next = nextProfileOrientation(*device, current, getRotatableOrientations());
    if (next == current) {
        if (error) *error = std::string("the game allows only ") + PlatformRules::orientationName(current) +
                            " (Screen.orientation / Screen.autorotateTo*)";
        return false;
    }
    return turnDevice(next);
}

bool editor::GameWindow::turnDevice(ScreenOrientation orientation) {
    loadSettings();
    const DeviceProfile* device = Platform::getSimulation().device;
    if (!device || !(getRotatableOrientations() & PlatformRules::orientationBit(orientation))) return false;
    settings.orientation = (int)orientation;
    Platform::setSimulatedDevice(device, orientation);
    editPassDirty = true;
    saveSettings();
    return true;
}

const std::vector<std::string>& editor::GameWindow::languagePresets() {
    static const std::vector<std::string> languages = {
        "", "en-US", "es-ES", "es-MX", "pt-BR", "fr-FR", "de-DE", "it-IT", "ja-JP", "ko-KR", "zh-Hans", "zh-Hant",
        "ru-RU", "ar-SA", "hi-IN"
    };
    return languages;
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
    applySimulation();
    editPassDirty = true;
    saveSettings();
}

int editor::GameWindow::getAspectCount() {
    return ASPECT_COUNT;
}

bool editor::GameWindow::computeAspect(float& aspect) const {
    // a simulated device has its own screen, turned as it is now
    if (const DeviceProfile* device = Platform::getSimulation().device) {
        int deviceWidth = 0, deviceHeight = 0;
        profileSize(*device, Platform::getSimulation().orientation, deviceWidth, deviceHeight);
        aspect = (float)deviceWidth / (float)std::max(1, deviceHeight);
        return true;
    }
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
    // also while the view is closed: the engine renders at the render size whatever is shown
    syncSafeArea();

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
    drawDeviceMenu();

    const DeviceProfile* device = Platform::getSimulation().device;
    std::string deviceAspect;
    if (device) {
        int deviceWidth = 0, deviceHeight = 0;
        profileSize(*device, Platform::getSimulation().orientation, deviceWidth, deviceHeight);
        deviceAspect = std::to_string(deviceWidth) + "x" + std::to_string(deviceHeight) + " (Device)";
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    ImGui::BeginDisabled(device != nullptr);
    if (ImGui::BeginCombo("##GameAspect", device ? deviceAspect.c_str() : aspectLabel(settings.aspect))) {
        for (int i = 0; i < ASPECT_COUNT; i++) {
            if (ImGui::Selectable(aspectLabel(i), settings.aspect == i)) {
                settings.aspect = i;
                editPassDirty = true;
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(device ? "The simulated device's screen sets the aspect" : "Aspect ratio of the game view");

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
    const char* networkLabel = ICON_FA_WIFI "##GameNetwork";
    const char* maximizeLabel = "Maximize On Play##GameMaximize";
    const char* statsLabel = "Stats##GameStats";
    const char* physicsLabel = "Physics##GamePhysicsDebug";
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    };
    auto groupWidth = [&]() {
        return buttonWidth(debuggerLabel) + buttonWidth(networkLabel) + buttonWidth(touchLabel) + buttonWidth(maximizeLabel) +
               buttonWidth(statsLabel) + buttonWidth(physicsLabel) + style.ItemSpacing.x * 5.0f;
    };
    ImGui::SameLine();
    const float contentRight = ImGui::GetWindowContentRegionMax().x;
    if (contentRight - groupWidth() < ImGui::GetCursorPosX()) {
        debuggerLabel = ICON_FA_BUG "##GameHtmlUiDebugger";
        maximizeLabel = ICON_FA_MAXIMIZE "##GameMaximize";
        statsLabel = ICON_FA_CHART_SIMPLE "##GameStats";
        physicsLabel = ICON_FA_CUBES "##GamePhysicsDebug";
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

    // Network: simulate no connection (HTTP requests to hosts other than localhost fail with
    // "offline"); works while playing, editor-only, back to Online when the editor restarts
    ImGui::SameLine();
    const bool offline = Http::isOfflineSimulated();
    if (toggleButton(networkLabel, offline, offline
            ? "Network: Offline (simulated). Http requests to hosts other than localhost fail with errorCode \"offline\"."
            : "Network: Online. Simulate a device without a connection (Http requests fail with \"offline\")")) {
        ImGui::OpenPopup("##GameNetworkMenu");
    }
    if (ImGui::BeginPopup("##GameNetworkMenu")) {
        if (ImGui::MenuItem("Online", nullptr, !offline)) Http::setOfflineSimulated(false);
        if (ImGui::MenuItem("Offline (simulated)", nullptr, offline)) Http::setOfflineSimulated(true);
        ImGui::Separator();
        ImGui::TextDisabled("localhost is never affected; not saved into builds");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    const bool simulateTouch = project->isSimulateTouch();
    ImGui::BeginDisabled(project->isAnyScenePlaying());
    if (toggleButton(touchLabel, simulateTouch, "Simulate touch: play as a touch device (the mouse acts as one finger). Release the left button with Alt held to keep that finger down: the next drag is a second finger (release Alt to lift the first)")) {
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

    ImGui::SameLine();
    // the runtime collider / joint / contact lines (Physics.debugDraw), for every playing scene
    const bool physicsShown = Physics::isDebugDraw();
    if (toggleButton(physicsLabel, physicsShown, "Physics: draw colliders, joints and contacts while playing")) {
        Physics::setDebugDraw(!physicsShown);
    }

    ImGui::SetCursorPosY(toolbarRowY + ImGui::GetTextLineHeightWithSpacing());
}

// Device simulation (Unity's Device Simulator): a phone or tablet, how it is turned and the
// language the game sees. Editor only: builds never see any of it.
void editor::GameWindow::drawDeviceMenu() {
    const PlatformSimulation& simulation = Platform::getSimulation();
    const DeviceProfile* device = simulation.device;
    const std::string languageLabel = settings.language.empty() ? std::string() : " | " + settings.language;
    const std::string label = std::string(ICON_FA_MOBILE_SCREEN_BUTTON " ") + (device ? device->name : "No Device") + languageLabel +
        "##GameDevice";

    const float comboWidth = std::min(ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetFrameHeight() +
                                      ImGui::GetStyle().FramePadding.x * 2.0f, Theme::dpi(240.0f));
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##GameDeviceCombo", label.c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::TextDisabled("Device");
        if (ImGui::Selectable("No Device", device == nullptr)) {
            settings.device.clear();
            applySimulation();
            saveSettings();
        }
        for (const DeviceProfile& profile : deviceProfiles()) {
            const std::string itemLabel = std::string(profile.name) + "##device_" + profile.id;
            if (ImGui::Selectable(itemLabel.c_str(), device == &profile)) {
                settings.device = profile.id;
                applySimulation();
                saveSettings();
            }
        }
        if (ImGui::Selectable("Custom...##device_custom", device == &customDeviceProfile())) {
            customDeviceEdit = settings.custom;
            customDeviceOpen = true;
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Language")) {
            for (const std::string& language : languagePresets()) {
                const char* name = language.empty() ? "System" : language.c_str();
                if (ImGui::MenuItem(name, nullptr, settings.language == language)) {
                    settings.language = language;
                    applySimulation();
                    saveSettings();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Custom...")) {
                snprintf(customLanguage, sizeof(customLanguage), "%s", settings.language.c_str());
                customLanguageOpen = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Device simulation: the phone or tablet the game sees (System.getDeviceModel, Screen.dpi, "
                          "Screen.orientation) and the language (System.getSystemLanguage). Editor only.");

    if (customDeviceOpen) {
        ImGui::OpenPopup("Custom Device##GameCustomDevice");
        customDeviceOpen = false;
    }
    if (ImGui::BeginPopup("Custom Device##GameCustomDevice")) {
        ImGui::TextUnformatted("Portrait screen and safe-area insets, in pixels:");
        CustomDeviceSettings& edit = customDeviceEdit;
        const float fieldWidth = Theme::dpi(90.0f);
        auto intField = [&](const char* label, int& value) {
            ImGui::SetNextItemWidth(fieldWidth);
            ImGui::InputInt(label, &value, 0);
        };
        auto floatField = [&](const char* label, float& value) {
            ImGui::SetNextItemWidth(fieldWidth);
            ImGui::InputFloat(label, &value, 0.0f, 0.0f, "%.0f");
        };
        intField("Width##CustomDevice", edit.width);
        ImGui::SameLine();
        intField("Height##CustomDevice", edit.height);
        floatField("Top##CustomDevice", edit.top);
        ImGui::SameLine();
        floatField("Bottom##CustomDevice", edit.bottom);
        floatField("Left##CustomDevice", edit.left);
        ImGui::SameLine();
        floatField("Right##CustomDevice", edit.right);
        floatField("Corner Radius##CustomDevice", edit.cornerRadius);
        edit.width = std::clamp(edit.width, 16, 16384);
        edit.height = std::clamp(edit.height, 16, 16384);
        for (float* value : {&edit.top, &edit.bottom, &edit.left, &edit.right, &edit.cornerRadius}) {
            if (!std::isfinite(*value) || *value < 0.0f) *value = 0.0f;
        }
        if (ImGui::Button("Apply##CustomDevice")) {
            settings.custom = edit;
            settings.device = "custom";
            applySimulation();
            saveSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##CustomDevice")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (customLanguageOpen) {
        ImGui::OpenPopup("Simulated Language##GameLanguage");
        customLanguageOpen = false;
    }
    if (ImGui::BeginPopup("Simulated Language##GameLanguage")) {
        ImGui::TextUnformatted("Language tag (BCP 47), e.g. es-MX, pt-BR, zh-Hant-TW:");
        ImGui::SetNextItemWidth(Theme::dpi(160.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##CustomLanguage", customLanguage, sizeof(customLanguage),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Apply") || entered) {
            settings.language = LocaleRules::normalize(customLanguage);
            applySimulation();
            saveSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    const uint8_t rotatable = getRotatableOrientations();
    const bool canRotate = device && nextProfileOrientation(*device, simulation.orientation, rotatable) != simulation.orientation;
    ImGui::BeginDisabled(!canRotate);
    if (ImGui::Button(ICON_FA_ROTATE "##GameRotate")) {
        rotateDevice();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!device) {
            ImGui::SetTooltip("Rotate the simulated device (pick a device first)");
        } else if (!canRotate) {
            ImGui::SetTooltip("The game allows only %s (Screen.orientation)", PlatformRules::orientationName(simulation.orientation));
        } else {
            ImGui::SetTooltip("Rotate the simulated device (now %s)", PlatformRules::orientationName(simulation.orientation));
        }
    }

    // the device's screen drawn over the game, and its safe area outlined
    ImGui::SameLine();
    ImGui::BeginDisabled(!device);
    if (toggleButton(ICON_FA_MOBILE "##GameDeviceOverlay", settings.deviceOverlay,
                     "Device Overlay: the simulated device's rounded corners, cutout and home indicator over the game")) {
        settings.deviceOverlay = !settings.deviceOverlay;
        saveSettings();
    }
    ImGui::SameLine();
    if (toggleButton(ICON_FA_VECTOR_SQUARE "##GameSafeAreaOutline", settings.safeAreaOutline,
                     "Safe Area: outline Screen.safeArea (the part of the screen the game's UI should keep to)")) {
        settings.safeAreaOutline = !settings.safeAreaOutline;
        saveSettings();
    }
    ImGui::EndDisabled();
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
        // the layout above is the simulated device's orientation now
        const ScreenOrientation simulatedOrientation = Platform::getSimulation().orientation;
        if (simulatedOrientation != layoutOrientation) {
            layoutOrientation = simulatedOrientation;
            editPassDirty = true;
            resized = true;
        }
        // stopped, Screen.orientation (eval_lua in edit mode) is the layout; while playing App
        // hands it to the engine with the size it goes with
        if (project->getPlaySessionSceneId() == NULL_PROJECT_SCENE) {
            Platform::setShownOrientation(layoutOrientation);
        }
        // the simulated insets go with this size, before App hands it to the engine
        if (resized) syncSafeArea();

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
            // the simulated device's screen over it (input is unaffected)
            const DeviceProfile* device = Platform::getSimulation().device;
            if (device && (settings.deviceOverlay || settings.safeAreaOutline)) {
                const Rect safe = safeAreaInImage(imageSize.x, imageSize.y);
                drawDeviceOverlay(drawList, imageMin, imageMax, *device, layoutOrientation, settings.safeAreaOutline, &safe,
                                  settings.deviceOverlay);
            }
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

    char lines[9][96];
    int lineCount = 5;
    snprintf(lines[0], sizeof(lines[0]), "Statistics");
    snprintf(lines[1], sizeof(lines[1]), "Graphics: %.1f FPS (%.1f ms)", fps, ms);
    snprintf(lines[2], sizeof(lines[2]), "Draw calls: %u   Tris: %llu", frameStats.drawCalls, (unsigned long long)frameStats.triangles);
    snprintf(lines[3], sizeof(lines[3]), "Screen: %dx%d", width, height);
    // Engine.targetFrameRate / vSyncCount of the playing game (-1: the platform's rate)
    const int targetFps = Engine::getTargetFrameRate();
    if (targetFps > 0) {
        snprintf(lines[4], sizeof(lines[4]), "Target: %d fps  VSync: %d", targetFps, Engine::getVSyncCount());
    } else {
        snprintf(lines[4], sizeof(lines[4]), "Target: default  VSync: %d", Engine::getVSyncCount());
    }
    if (const DeviceProfile* device = Platform::getSimulation().device) {
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "Device: %s, %s", device->name,
                 PlatformRules::orientationName(Platform::getSimulation().orientation));
        lineCount++;
    }
    // Screen.safeArea, canvas units
    const Rect safe = Screen::getSafeArea();
    snprintf(lines[lineCount], sizeof(lines[lineCount]), "Safe area: %g,%g %gx%g", std::round(safe.getX()), std::round(safe.getY()),
             std::round(safe.getWidth()), std::round(safe.getHeight()));
    lineCount++;
    // the scene's HTML UI documents (their offscreen passes are not in the counters above)
    UIDocumentSystem* documents = (sceneProject && sceneProject->scene) ? sceneProject->scene->getSystem<UIDocumentSystem>().get() : nullptr;
    if (documents && documents->hasDocuments()) {
        const HtmlUIRenderStats& ui = documents->getRenderStats();
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "HTML UI: %u passes, %u draws, %u pipelines", ui.passes, ui.drawCalls, ui.pipelineApplies);
        lineCount++;
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
