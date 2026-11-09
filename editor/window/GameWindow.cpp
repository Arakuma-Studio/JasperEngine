// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "GameWindow.h"
#include "window/ContactSheetWindow.h"
#include "audio/AudioCapture.h"
#include "render/SystemRender.h"
#include "util/UIProbe.h"
#include "recorder/RecorderSession.h"
#include "widget/ColorField.h"
#include "Theme.h"
#include "texture/TextureCookQueue.h"
#include "profiler/FrameTimes.h"

#include "imgui_internal.h"
#include "Engine.h"
#include "device/Device.h"
#include "device/QualityResolver.h"
#include "Platform.h"
#include "Screen.h"
#include "util/PlatformRules.h"
#include "window/DeviceProfiles.h"
#include "Backend.h"
#include "Theme.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/CameraControlSystem.h"
#include "registry/PhysicsLayers.h"
#include "Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "net/Http.h"
#include "shortcuts/CommandCatalog.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/RenderSystem.h"
#include "render/SystemRender.h"
#include "render/RenderStats.h"
#include "pixelart/HitStop.h"
#include "render/SceneDrawModes.h"
#include "render/RenderUtil.h"
#include "window/EditorModal.h"
#include "pixelart/ScreenTransition.h"

#include <algorithm>
#include <cstdio>
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
    // the Pixel Scale choices (S41 R20): Auto, 1x .. 8x
    constexpr int MAX_PIXEL_SCALE = 8;

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

editor::GameWindow::~GameWindow() = default;

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
    settings.pixelScale = std::clamp(settings.pixelScale, 0, MAX_PIXEL_SCALE);
    Engine::setPixelPerfectForcedScale(settings.pixelScale);
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
    // a resolution preset's dpi (Screen.dpi, Constant Physical Size); a device has its own
    Platform::setSimulatedDpi(device ? 0.0f : (settings.resolution.empty() ? 0.0f : settings.resolutionDpi));
    editPassDirty = true;
}

bool editor::GameWindow::getFixedResolution(int& fixedWidth, int& fixedHeight, float& dpi) const {
    // a simulated device renders at its screen's pixels, turned as it is now
    if (const DeviceProfile* device = Platform::getSimulation().device) {
        profileSize(*device, Platform::getSimulation().orientation, fixedWidth, fixedHeight);
        dpi = device->dpi;
        return fixedWidth > 0 && fixedHeight > 0;
    }
    if (!settings.resolution.empty() && isValidGameResolution(settings.resolutionWidth, settings.resolutionHeight, settings.resolutionDpi)) {
        fixedWidth = settings.resolutionWidth;
        fixedHeight = settings.resolutionHeight;
        dpi = settings.resolutionDpi;
        return true;
    }
    return false;
}

bool editor::GameWindow::setResolution(const std::string& name, std::string* error) {
    loadSettings();
    if (name.empty() || name == "free") {
        settings.resolution.clear();
        settings.resolutionWidth = 0;
        settings.resolutionHeight = 0;
        settings.resolutionDpi = 0.0f;
    } else {
        GameResolution resolution;
        std::string why;
        if (!findGameResolution(name, project->getCustomResolutions(), resolution, why)) {
            if (error) *error = why;
            return false;
        }
        settings.resolution = resolution.name;
        settings.resolutionWidth = resolution.width;
        settings.resolutionHeight = resolution.height;
        settings.resolutionDpi = resolution.dpi;
    }
    applySimulation();
    saveSettings();
    return true;
}

bool editor::GameWindow::addCustomResolution(const GameResolution& resolution, std::string* error) {
    if (resolution.name.empty() || !isValidGameResolution(resolution.width, resolution.height, resolution.dpi)) {
        if (error) *error = "a custom resolution needs a name, 16..16384 pixels a side and a dpi of 0..2000";
        return false;
    }
    for (const GameResolution& builtIn : builtInGameResolutions()) {
        if (builtIn.name == resolution.name) {
            if (error) *error = "\"" + resolution.name + "\" is a built-in resolution";
            return false;
        }
    }
    std::vector<GameResolution> custom = project->getCustomResolutions();
    custom.erase(std::remove_if(custom.begin(), custom.end(), [&](const GameResolution& r) { return r.name == resolution.name; }), custom.end());
    custom.push_back(resolution);
    project->setCustomResolutions(custom);
    project->saveWorkspaceFile();
    return true;
}

void editor::GameWindow::removeCustomResolution(const std::string& name) {
    std::vector<GameResolution> custom = project->getCustomResolutions();
    custom.erase(std::remove_if(custom.begin(), custom.end(), [&](const GameResolution& r) { return r.name == name; }), custom.end());
    project->setCustomResolutions(custom);
    project->saveWorkspaceFile();
    if (settings.resolution == name) setResolution("");
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

void editor::GameWindow::setRenderSizeOverride(int w, int h) {
    overrideWidth = w > 0 && h > 0 ? w : 0;
    overrideHeight = w > 0 && h > 0 ? h : 0;
    editPassDirty = true;
}

int editor::GameWindow::getRenderWidth() const {
    if (overrideWidth > 0) {
        return overrideWidth;
    }
    if (width > 0 && height > 0) {
        return width;
    }
    // the tab hidden: a fixed resolution (a device, a preset) still renders at its size
    int fixedWidth = 0, fixedHeight = 0;
    float dpi = 0.0f;
    if (getFixedResolution(fixedWidth, fixedHeight, dpi)) {
        return fixedWidth;
    }
    return std::max(1, (int)project->getCanvasWidth());
}

int editor::GameWindow::getRenderHeight() const {
    if (overrideHeight > 0) {
        return overrideHeight;
    }
    if (width > 0 && height > 0) {
        return height;
    }
    int fixedWidth = 0, fixedHeight = 0;
    float dpi = 0.0f;
    if (getFixedResolution(fixedWidth, fixedHeight, dpi)) {
        return fixedHeight;
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
    settings.pixelScale = std::clamp(settings.pixelScale, 0, MAX_PIXEL_SCALE);
    Engine::setPixelPerfectForcedScale(settings.pixelScale);
    applySimulation();
    editPassDirty = true;
    saveSettings();
}

void editor::GameWindow::toggleMaximizeOnPlay() {
    loadSettings();
    settings.maximizeOnPlay = !settings.maximizeOnPlay;
    saveSettings();
}

bool editor::GameWindow::isMaximizeOnPlay() {
    loadSettings();
    return settings.maximizeOnPlay;
}

int editor::GameWindow::getAspectCount() {
    return ASPECT_COUNT;
}

bool editor::GameWindow::computeAspect(float& aspect) const {
    // a simulated device's screen, or a fixed resolution
    int fixedWidth = 0, fixedHeight = 0;
    float dpi = 0.0f;
    if (getFixedResolution(fixedWidth, fixedHeight, dpi)) {
        aspect = (float)fixedWidth / (float)std::max(1, fixedHeight);
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

    if (contactSheet) contactSheet->show();

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
        if (!maximizedShown) {
            maximizeHintUntil = ImGui::GetTime() + 3.0;
        }
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
            // how to get the editor back, fading out over the last second of 3
            const double left = maximizeHintUntil - ImGui::GetTime();
            if (left > 0.0) {
                // the key is the game's while it has the keyboard: the menu item always answers
                const std::string keys = CommandCatalog::editor().menuShortcutText("playmode.maximizeOnPlay");
                const std::string hint = "Maximize On Play: Edit > Maximize On Play shows the editor" +
                                         (keys.empty() ? std::string() : " (" + keys + " outside the game)");
                const float alpha = static_cast<float>(std::min(1.0, left));
                const ImVec2 size = ImGui::CalcTextSize(hint.c_str());
                const ImVec2 pad(Theme::dpi(8.0f), Theme::dpi(5.0f));
                const ImVec2 max(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - Theme::dpi(12.0f),
                                 ImGui::GetWindowPos().y + Theme::dpi(12.0f) + size.y + pad.y * 2.0f);
                const ImVec2 min(max.x - size.x - pad.x * 2.0f, max.y - size.y - pad.y * 2.0f);
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(min, max, IM_COL32(20, 20, 20, (int)(200 * alpha)), Theme::dpi(4.0f));
                drawList->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(235, 235, 235, (int)(255 * alpha)), hint.c_str());
            }
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

bool editor::GameWindow::shownPixelPerfect(PixelPerfectViewRequest& request) const {
    const uint32_t sceneId = getTargetSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return false;
    return Engine::getPixelPerfectRequest(sceneProject->scene, project->getGameCamera(sceneId), request);
}

void editor::GameWindow::drawPixelScale(const PixelPerfectViewRequest& request) {
    // the largest k the view fits, at the view's own size (Auto's)
    PixelPerfectViewRequest autoRequest = request;
    const jasperengine::pixelart::PixelPerfectLayout fit = Engine::computePixelPerfectView(autoRequest, width, height, 0);
    const jasperengine::pixelart::PixelPerfectLayout shown = Engine::computePixelPerfectView(request, width, height, settings.pixelScale);

    ImGui::SameLine(0, Theme::dpi(12.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pixel Scale");
    ImGui::SameLine();
    char current[16];
    if (settings.pixelScale == 0) std::snprintf(current, sizeof(current), "Auto");
    else std::snprintf(current, sizeof(current), "%dx", settings.pixelScale);
    ImGui::SetNextItemWidth(Theme::dpi(64.0f));
    if (ImGui::BeginCombo("##GamePixelScale", current)) {
        if (ImGui::Selectable("Auto", settings.pixelScale == 0)) {
            settings.pixelScale = 0;
            applySettings(settings);
        }
        for (int scale = 1; scale <= MAX_PIXEL_SCALE; scale++) {
            char label[16];
            std::snprintf(label, sizeof(label), "%dx", scale);
            ImGui::BeginDisabled(scale > fit.scale);
            if (ImGui::Selectable(label, settings.pixelScale == scale)) {
                settings.pixelScale = scale;
                applySettings(settings);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Screen pixels per base pixel of the Pixel Perfect Camera (Auto: the largest that fits). 1x shows "
                          "the base resolution at one physical pixel per pixel: check pixel art at 1x.");
    if (shown.baseWidth > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d x%d", shown.baseWidth, shown.baseHeight, shown.scale);
    }
}

void editor::GameWindow::drawTransitionMenu() {
    using namespace jasperengine::pixelart;
    if (!ImGui::BeginPopup("##GameTransitionMenu"))
        return;
    const TransitionDefaults& defaults = ScreenTransition::getDefaults();
    if (!transitionPreviewSet) {
        transitionPreviewDuration = defaults.duration;
        transitionPreviewColor = defaults.color;
        std::snprintf(transitionPreviewRamp, sizeof(transitionPreviewRamp), "%s", defaults.ramp.c_str());
        transitionPreviewSet = true;
    }
    static const char* const labels[kTransitionKindCount] = {"Iris", "Blinds", "Slash", "Pixel Dissolve", "CRT Off", "Burn Away", "Fade"};
    for (int k = 0; k < kTransitionKindCount; k++) {
        if (ImGui::MenuItem(labels[k], nullptr, ScreenTransition::isActive() && ScreenTransition::getKind() == (TransitionKind)k)) {
            TransitionOptions options = TransitionOptions::fromDefaults();
            options.ink = transitionPreviewColor;
            options.ramp = transitionPreviewRamp;
            ScreenTransition::play((TransitionKind)k, transitionPreviewDuration, options);
            editPassDirty = true;
        }
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::DragFloat("Duration", &transitionPreviewDuration, 0.01f, 0.05f, 10.0f, "%.2f s");
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::ColorEdit3("Colour", &transitionPreviewColor.x);
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::InputTextWithHint("Ramp", "built-in ember ramp", transitionPreviewRamp, sizeof(transitionPreviewRamp));
    ImGui::SetItemTooltip("A palette ramp, \"<palette>.palette#ramp:<name>\"");
    ImGui::TextDisabled("Kind, rim and pixel size: Project Settings > Graphics > Screen Transitions");
    ImGui::EndPopup();
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

    // the aspect or the fixed resolution, and how much the image is shrunk to fit
    std::string sizeLabel = device ? deviceAspect : aspectLabel(settings.aspect);
    if (!device && !settings.resolution.empty()) {
        sizeLabel = std::to_string(settings.resolutionWidth) + "x" + std::to_string(settings.resolutionHeight);
    }
    if (fixed && std::fabs(fitScale - 1.0f) > 0.005f) {
        char scaleText[32];
        std::snprintf(scaleText, sizeof(scaleText), " (%.2fx)", fitScale);
        sizeLabel += scaleText;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(170.0f));
    ImGui::BeginDisabled(device != nullptr);
    if (ImGui::BeginCombo("##GameAspect", sizeLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::SeparatorText("Aspect");
        for (int i = 0; i < ASPECT_COUNT; i++) {
            if (ImGui::Selectable(aspectLabel(i), settings.resolution.empty() && settings.aspect == i)) {
                settings.aspect = i;
                setResolution("");
                editPassDirty = true;
                saveSettings();
            }
        }
        // fixed sizes: the game renders at them and the image is fitted to the view (Scale zooms)
        ImGui::SeparatorText("Resolutions");
        for (const GameResolution& resolution : builtInGameResolutions()) {
            if (ImGui::Selectable(gameResolutionLabel(resolution).c_str(), settings.resolution == resolution.name)) {
                setResolution(resolution.name);
            }
        }
        std::string removeName;
        for (const GameResolution& resolution : project->getCustomResolutions()) {
            ImGui::PushID(resolution.name.c_str());
            if (ImGui::Selectable(gameResolutionLabel(resolution).c_str(), settings.resolution == resolution.name)) {
                setResolution(resolution.name);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove")) removeName = resolution.name;
                ImGui::EndPopup();
            }
            ImGui::SetItemTooltip("Right-click to remove");
            ImGui::PopID();
        }
        if (!removeName.empty()) removeCustomResolution(removeName);
        if (ImGui::Selectable(ICON_FA_PLUS " Add custom...", false)) {
            customPopupRequested = true;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(device ? "The simulated device's screen sets the resolution"
                                 : "Aspect ratio, or a fixed resolution the game renders at (fitted to the view)");
    drawCustomResolutionPopup();

    // S41 R20: a shown scene drawn through a Pixel Perfect Camera gets the Pixel Scale, and its image
    // is magnified by whole numbers only
    PixelPerfectViewRequest pixelRequest;
    const bool pixelPerfect = shownPixelPerfect(pixelRequest);
    if (pixelPerfect) {
        drawPixelScale(pixelRequest);
    }

    ImGui::SameLine(0, Theme::dpi(12.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderFloat("##GameScale", &settings.scale, MIN_SCALE, MAX_SCALE, pixelPerfect ? "%.0fx" : "%.1fx");
    if (pixelPerfect) {
        settings.scale = std::round(settings.scale);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        saveSettings();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        settings.scale = 1.0f;
        saveSettings();
    }
    ImGui::SetItemTooltip("Zoom into the game image (double-click resets)");

    // Preview Atmosphere Quality: the tier of volumetric fog and clouds this view shows
    static const char* const atmosphereLabels[] = {"Project", "Off", "Low", "Medium", "High"};
    ImGui::SameLine(0, Theme::dpi(12.0f));
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    const int atmosphereIndex = std::clamp(settings.atmosphereQuality + 1, 0, 4);
    if (ImGui::BeginCombo("##GameAtmosphere", atmosphereLabels[atmosphereIndex])) {
        for (int i = 0; i < 5; i++) {
            if (ImGui::Selectable(atmosphereLabels[i], atmosphereIndex == i)) {
                settings.atmosphereQuality = i - 1;
                editPassDirty = true;
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Preview Atmosphere Quality: the volumetric fog and clouds tier of this view (Project: the project's "
                          "desktop Atmosphere Quality). A script's Engine.setAtmosphereQuality still wins.");

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

    ImGui::SameLine(0, Theme::dpi(12.0f));
    drawDeviceTierMenu();

    // S48 R40: a screen transition played over the view; left of the right-aligned group, which
    // runs off a narrow view's edge
    ImGui::SameLine(0, Theme::dpi(12.0f));
    if (toggleButton(ICON_FA_CIRCLE_HALF_STROKE "##GameTransition", jasperengine::pixelart::ScreenTransition::isActive(),
                     "Transition: play a screen transition (cover, hold, reveal) over the Game view, in edit mode or Play")) {
        ImGui::OpenPopup("##GameTransitionMenu");
    }
    drawTransitionMenu();

    // S48 R40: the shown scene's one selected GameObject drawn at N times on backdrops, with checks
    ImGui::SameLine();
    {
        const uint32_t shownId = getTargetSceneId();
        const std::vector<Entity> selected = project->getSelectedEntities(shownId);
        ImGui::BeginDisabled(selected.size() != 1);
        if (ImGui::Button("Contact Sheet...##GameContactSheet")) {
            if (!contactSheet) contactSheet = std::make_unique<ContactSheetWindow>(project);
            contactSheet->open(shownId, selected.front());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(selected.size() == 1 ? "Capture the selected GameObject at fixed times into a labelled contact sheet, with effect checks"
                                                   : "Select one GameObject of the shown scene to capture it");
    }

    // Right side toggles, as icons when the view is too narrow for their names
    const char* debuggerLabel = "HTML UI Debugger##GameHtmlUiDebugger";
    const char* touchLabel = ICON_FA_MOBILE_SCREEN "##GameTouch";
    const char* networkLabel = ICON_FA_WIFI "##GameNetwork";
    const char* maximizeLabel = "Maximize On Play##GameMaximize";
    const char* statsLabel = "Stats##GameStats";
    // Simulate platform textures (S14 R26): the family the views render cooked textures with
    TextureCookQueue& cooks = TextureCookQueue::get();
    const TextureFamily viewFamily = cooks.getViewFamily();
    std::string texturesLabel = std::string(ICON_FA_IMAGE " ") + textureFamilyLabel(viewFamily) + "##GameTextures";
    const char* physicsLabel = "Physics##GamePhysicsDebug";
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    };
    auto groupWidth = [&]() {
        return buttonWidth(texturesLabel.c_str()) + buttonWidth(debuggerLabel) + buttonWidth(networkLabel) +
               buttonWidth(touchLabel) + buttonWidth(maximizeLabel) + buttonWidth(statsLabel) + buttonWidth(physicsLabel) + style.ItemSpacing.x * 6.0f;
    };
    ImGui::SameLine();
    const float contentRight = ImGui::GetWindowContentRegionMax().x;
    if (contentRight - groupWidth() < ImGui::GetCursorPosX()) {
        debuggerLabel = ICON_FA_BUG "##GameHtmlUiDebugger";
        maximizeLabel = ICON_FA_MAXIMIZE "##GameMaximize";
        statsLabel = ICON_FA_CHART_SIMPLE "##GameStats";
        texturesLabel = ICON_FA_IMAGE "##GameTextures";
        physicsLabel = ICON_FA_CUBES "##GamePhysicsDebug";
    }
    const float rightX = contentRight - groupWidth();
    if (rightX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(rightX);
    }

    ImGui::BeginDisabled(cooks.getProject() != project);
    if (toggleButton(texturesLabel.c_str(), viewFamily != TextureFamily::Desktop,
                     "Simulate platform textures: render with the Desktop, Mobile or Web cook of every texture (decoded "
                     "on the CPU when this GPU cannot sample it), to see their artefacts before an export")) {
        ImGui::OpenPopup("##GameTexturesPopup");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##GameTexturesPopup")) {
        for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}) {
            if (family == TextureFamily::AndroidEtc2 && !project->getTextureSettings().androidEtc2Fallback) continue;
            if (ImGui::MenuItem(textureFamilyLabel(family), nullptr, family == viewFamily)) cooks.setViewFamily(family);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();

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

// "Add custom...": a named size and dpi, kept in this project's editor workspace
void editor::GameWindow::drawCustomResolutionPopup() {
    if (customPopupRequested) {
        customPopupRequested = false;
        std::snprintf(customName, sizeof(customName), "Custom %zu", project->getCustomResolutions().size() + 1);
        ImGui::OpenPopup("Add Game View Resolution");
    }
    if (!EditorModal::begin("Add Game View Resolution", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    EditorModal::message("A custom resolution for the Game view.");

    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    EditorModal::inputText("Name", "name", customName, sizeof(customName));
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    EditorModal::inputInt("Width", "width", &customWidth);
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    EditorModal::inputInt("Height", "height", &customHeight);
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    EditorModal::inputFloat("DPI", "dpi", &customDpi, 1.0f, 10.0f, "%.0f");
    ImGui::SetItemTooltip("Screen.dpi while this resolution is chosen (0: the computer's); Constant Physical Size uses it");
    customWidth = std::clamp(customWidth, 16, 16384);
    customHeight = std::clamp(customHeight, 16, 16384);
    customDpi = std::clamp(customDpi, 0.0f, 2000.0f);

    GameResolution resolution;
    resolution.name = customName;
    resolution.width = customWidth;
    resolution.height = customHeight;
    resolution.dpi = customDpi;
    std::string error;
    ImGui::BeginDisabled(resolution.name.empty());
    if (EditorModal::button("Add", "ok")) {
        if (addCustomResolution(resolution, &error)) {
            setResolution(resolution.name);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
    EditorModal::error(error);
    if (!error.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", error.c_str());
    ImGui::EndPopup();
}

// Device simulation (Unity's Device Simulator): a phone or tablet, how it is turned and the
// language the game sees. Editor only: builds never see any of it.
DeviceSimulation editor::GameWindow::getDeviceSimulation() const {
    DeviceSimulation simulation;
    if (settings.deviceTier >= 0)
        simulation.tier = (DeviceTier)std::clamp(settings.deviceTier, 0, DEVICE_TIER_COUNT - 1);
    if (settings.devicePlatform >= 0)
        simulation.group = (DevicePlatformGroup)std::clamp(settings.devicePlatform, 0, DEVICE_PLATFORM_GROUP_COUNT - 1);
    else if (Platform::getSimulation().device)
        simulation.group = DevicePlatformGroup::Mobile;
    if (settings.deviceThermal >= 0)
        simulation.thermal = (ThermalState)std::clamp(settings.deviceThermal, 0, THERMAL_STATE_COUNT - 1);
    return simulation;
}

void editor::GameWindow::drawDeviceTierMenu() {
    const DeviceSimulation simulation = getDeviceSimulation();
    std::string label = std::string("Device: ") + (simulation.tier ? DeviceNames::tierLabel(*simulation.tier) : "Auto");
    if (settings.devicePlatform >= 0)
        label += std::string(" | ") + DeviceNames::groupLabel(*simulation.group);
    if (simulation.thermal)
        label += std::string(" | ") + DeviceNames::thermalLabel(*simulation.thermal);
    label += "##GameDeviceTier";
    ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetFrameHeight() +
                                     ImGui::GetStyle().FramePadding.x * 2.0f, Theme::dpi(260.0f)));
    if (ImGui::BeginCombo("##GameDeviceTierCombo", label.c_str(), ImGuiComboFlags_HeightLarge)) {
        auto item = [this](const char* name, int& field, int value) {
            if (ImGui::Selectable(name, field == value)) {
                field = value;
                saveSettings();
            }
        };
        ImGui::TextDisabled("Device Tier");
        item("Auto##tier", settings.deviceTier, -1);
        for (int t = 0; t < DEVICE_TIER_COUNT; t++)
            item((std::string(DeviceNames::tierLabel((DeviceTier)t)) + "##tier").c_str(), settings.deviceTier, t);
        ImGui::Separator();
        ImGui::TextDisabled("Platform");
        item(Platform::getSimulation().device ? "Auto (Mobile)##platform" : "Auto (Desktop)##platform", settings.devicePlatform, -1);
        for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++)
            item((std::string(DeviceNames::groupLabel((DevicePlatformGroup)g)) + "##platform").c_str(), settings.devicePlatform, g);
        ImGui::Separator();
        ImGui::TextDisabled("Thermal State");
        item("This Machine##thermal", settings.deviceThermal, -1);
        for (int t = 0; t < THERMAL_STATE_COUNT; t++)
            item((std::string(DeviceNames::thermalLabel((ThermalState)t)) + "##thermal").c_str(), settings.deviceThermal, t);
        ImGui::EndCombo();
    }
    const DeviceDetection& detection = Device::getDetection();
    ImGui::SetItemTooltip("Device simulation in Play: the Device Tier whose overrides apply (Project Settings > Quality > Device "
                          "Tiers), the platform family whose table it reads and the thermal state the game sees. Auto applies no "
                          "tier in the editor; this machine detects %s.", detection.detected ? detection.reason.c_str() : "nothing yet");
}

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
        floatField("DPI##CustomDevice", edit.dpi);
        ImGui::SetItemTooltip("Screen.dpi on this screen (0: the computer's); Constant Physical Size uses it");
        edit.width = std::clamp(edit.width, 16, 16384);
        edit.height = std::clamp(edit.height, 16, 16384);
        edit.dpi = std::min(edit.dpi, 2000.0f);
        for (float* value : {&edit.top, &edit.bottom, &edit.left, &edit.right, &edit.cornerRadius, &edit.dpi}) {
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
        const bool overridden = overrideWidth > 0 && overrideHeight > 0;
        if (overridden) aspect = (float)overrideWidth / (float)overrideHeight;
        if ((overridden || computeAspect(aspect)) && avail.x > 0.0f && avail.y > 0.0f) {
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
        // a Pixel Perfect Camera's view is laid out over physical pixels (R20): the framebuffer
        // gets one pixel per physical display pixel even where the backend renders the scene at
        // the DPI scale over a 1:1 framebuffer (Linux HiDPI), which would resample it
        PixelPerfectViewRequest pixelRequest;
        const bool pixelPerfect = shownPixelPerfect(pixelRequest);
        if (pixelPerfect) {
            engineScale = ImVec2(fbScale.x > 0.0f ? fbScale.x : 1.0f, fbScale.y > 0.0f ? fbScale.y : 1.0f);
        }

        // the Recorder's size: the image is that many engine pixels over the area
        if (overridden && areaSize.x > 0.0f && areaSize.y > 0.0f) {
            engineScale = ImVec2((float)overrideWidth / areaSize.x, (float)overrideHeight / areaSize.y);
        }
        int newWidth = overridden ? overrideWidth : (areaSize.x > 0.0f ? std::max(1, (int)std::lround(areaSize.x * engineScale.x)) : 0);
        int newHeight = overridden ? overrideHeight : (areaSize.y > 0.0f ? std::max(1, (int)std::lround(areaSize.y * engineScale.y)) : 0);
        // a fixed resolution renders at its own size, the image fitted to the area (the
        // Recorder's size wins over it)
        int fixedWidth = 0, fixedHeight = 0;
        float fixedDpi = 0.0f;
        fixed = !overridden && getFixedResolution(fixedWidth, fixedHeight, fixedDpi) && areaSize.x > 0.0f && areaSize.y > 0.0f;
        if (fixed) {
            newWidth = fixedWidth;
            newHeight = fixedHeight;
            // the image's screen pixels per game pixel (the toolbar's "(0.48x)"), then engine
            // pixels per ImGui pixel of the fitted image
            fitScale = areaSize.x * engineScale.x / (float)fixedWidth;
            engineScale = ImVec2((float)fixedWidth / areaSize.x, (float)fixedHeight / areaSize.y);
        } else {
            fitScale = 1.0f;
        }
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
        // the Scene view lays UI out for what this view shows (Canvas Scaler)
        if (width > 0 && height > 0) {
            // Screen.dpi as Play reads it (the device's, a resolution preset's, else the host's)
            project->setEditViewSize((float)width, (float)height, Screen::getDpi());
        }
        // the simulated insets go with this size, before App hands it to the engine
        if (resized) syncSafeArea();

        // Scale zooms into the image around its centre; the child clips the rest
        float zoom = std::clamp(settings.scale, MIN_SCALE, MAX_SCALE);
        ImVec2 imageSize(areaSize.x * zoom, areaSize.y * zoom);
        ImVec2 imageMin(areaMin.x + (areaSize.x - imageSize.x) * 0.5f,
                        areaMin.y + (areaSize.y - imageSize.y) * 0.5f);
        if (pixelPerfect && engineScale.x > 0.0f && engineScale.y > 0.0f && width > 0 && height > 0) {
            // one framebuffer pixel per physical pixel (times a whole zoom): the image's size from
            // the framebuffer's, its corner on a physical pixel, no resampling (R20)
            zoom = std::round(zoom);
            imageSize = ImVec2((float)width / engineScale.x * zoom, (float)height / engineScale.y * zoom);
            imageMin = ImVec2(std::round((areaMin.x + (areaSize.x - imageSize.x) * 0.5f) * engineScale.x) / engineScale.x,
                              std::round((areaMin.y + (areaSize.y - imageSize.y) * 0.5f) * engineScale.y) / engineScale.y);
        }
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
            // pixel art magnifies with nearest sampling (the backend's standard callbacks, where it has them)
            const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
            const bool nearest = pixelPerfect && platformIO.DrawCallback_SetSamplerNearest && platformIO.DrawCallback_SetSamplerLinear;
            if (nearest) drawList->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
            drawList->AddImage(texture, imageMin, imageMax);
            if (nearest) drawList->AddCallback(platformIO.DrawCallback_SetSamplerLinear, nullptr);
            // the simulated device's screen over it (input is unaffected)
            const DeviceProfile* device = Platform::getSimulation().device;
            if (device && (settings.deviceOverlay || settings.safeAreaOutline)) {
                const Rect safe = safeAreaInImage(imageSize.x, imageSize.y);
                drawDeviceOverlay(drawList, imageMin, imageMax, *device, layoutOrientation, settings.safeAreaOutline, &safe,
                                  settings.deviceOverlay);
            }
            drawList->PopClipRect();
            // the eyedropper reads this image's texels (S27 R4)
            ui::registerViewImage(ImGui::GetWindowViewport()->ID, imageMin, imageMax, &framebuffer);
        }
        // the Recorder's overlay: drawn over the view, never into the recorded image (S27 R26)
        const std::string recording = recorder::RecorderSession::get().overlayText();
        if (!recording.empty()) {
            const ImVec2 at(areaMin.x + Theme::dpi(8.0f), areaMin.y + Theme::dpi(8.0f));
            const ImVec2 size = ImGui::CalcTextSize(recording.c_str());
            drawList->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + size.x + 4.0f, at.y + size.y + 2.0f), IM_COL32(0, 0, 0, 170), 3.0f);
            drawList->AddCircleFilled(ImVec2(at.x + 5.0f, at.y + size.y * 0.5f), 4.0f, IM_COL32(230, 40, 40, 255));
            drawList->AddText(at, IM_COL32(255, 70, 70, 255), recording.c_str());
        }

        // a capture mutes the device: say why the game is silent (S35 R31)
        if (AudioCapture::isActive()) {
            static const char* const capturing = "Capturing audio: output muted";
            const ImVec2 at(areaMin.x + Theme::dpi(8.0f), areaMin.y + areaSize.y - ImGui::GetTextLineHeight() - Theme::dpi(8.0f));
            const ImVec2 size = ImGui::CalcTextSize(capturing);
            drawList->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + size.x + 4.0f, at.y + size.y + 2.0f), IM_COL32(0, 0, 0, 160), 3.0f);
            drawList->AddText(at, IM_COL32(255, 255, 255, 255), capturing);
            UIProbe::rect("game.capturingAudio", at.x, at.y, at.x + size.x, at.y + size.y);
        }

        // the Transition menu's edit-mode preview: the scene redraws every frame while it runs
        // (not the edit pass's dirty flag: a screenshot waits for that to settle)
        if (!running && sceneProject && sceneProject->playState == ScenePlayState::STOPPED && jasperengine::pixelart::ScreenTransition::isActive())
            sceneProject->needUpdateRender = true;

        // the Camera Shaker Inspector's edit-mode preview: redrawn every frame while it shakes
        if (!running && sceneProject && sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED) {
            std::shared_ptr<CameraControlSystem> cameras = sceneProject->scene->findSystem<CameraControlSystem>();
            if (cameras && cameras->isPreviewing()) {
                sceneProject->needUpdateRender = true;
                editPassDirty = true;
                const char* badge = ICON_FA_BURST "  Shake preview";
                const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
                const ImVec2 pad = Theme::dpi(ImVec2(8.0f, 4.0f));
                const ImVec2 badgeMin(areaMin.x + Theme::dpi(8.0f), areaMin.y + Theme::dpi(8.0f));
                const ImVec2 badgeMax(badgeMin.x + badgeSize.x + pad.x * 2.0f, badgeMin.y + badgeSize.y + pad.y * 2.0f);
                drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(20, 20, 20, 210), Theme::dpi(3.0f));
                drawList->AddText(ImVec2(badgeMin.x + pad.x, badgeMin.y + pad.y), IM_COL32(255, 200, 80, 255), badge);
            }
        }

        if (message) {
            const ImVec2 textSize = ImGui::CalcTextSize(message);
            drawList->AddText(ImVec2(canvasMin.x + (avail.x - textSize.x) * 0.5f, canvasMin.y + (avail.y - textSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
        }

        const bool hovered = ImGui::IsWindowHovered();

        if (running && sceneProject->playState == ScenePlayState::PLAYING) {
            // Escape is the game's while it plays here (Unity): ImGui's keyboard navigation
            // would take it to leave this child window (its focus lost before PlayInput saw the
            // key). The claim applies from the next frame on and locks nothing, so every editor
            // reader of the key with the default owner test still sees the press (a captured
            // cursor's release among them)
            if (focused) {
                ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##GameCanvasKeys"));
            }
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

    char lines[16][112];
    int lineCount = 5;
    // the view's vertices and detail levels (spec S54 R12): the frame while playing, else the Game view's own
    const bool playing = project && project->isPlaySessionActive();
    const RenderStats viewStats = SystemRender::getRenderStats(playing ? SystemRender::FRAME_STATS_FRAME : GAME_VIEW_ID);
    RenderSystem* render = (sceneProject && sceneProject->scene) ? sceneProject->scene->getSystem<RenderSystem>().get() : nullptr;
    const LodFrameStats lod = !render ? LodFrameStats() : (playing ? render->getLodStats() : render->getViewLodStats(GAME_VIEW_ID));
    std::vector<std::string> meshLines;
    appendMeshStatsLines(meshLines, playing ? frameStats.vertices : viewStats.vertices, lod, sceneProject && sceneProject->scene && sceneProject->scene->isMeshLodEnabled());
    snprintf(lines[0], sizeof(lines[0]), "Statistics");
    snprintf(lines[1], sizeof(lines[1]), "Graphics: %.1f FPS (%.1f ms)", fps, ms);
    snprintf(lines[2], sizeof(lines[2]), "Draw calls: %u   SetPass calls: %u   Tris: %llu", frameStats.drawCalls, frameStats.setPassCalls,
             (unsigned long long)frameStats.triangles);
    snprintf(lines[3], sizeof(lines[3]), "Screen: %dx%d", width, height);
    // Engine.targetFrameRate / vSyncCount of the playing game (-1: the platform's rate)
    const int targetFps = Engine::getTargetFrameRate();
    if (targetFps > 0) {
        snprintf(lines[4], sizeof(lines[4]), "Target: %d fps  VSync: %d", targetFps, Engine::getVSyncCount());
    } else {
        snprintf(lines[4], sizeof(lines[4]), "Target: default  VSync: %d", Engine::getVSyncCount());
    }
    // the main view's and the shadow maps' share (S55 R7), and what GPU Instancing and the visible
    // lists saved (Unity's "Saved by batching")
    const RenderStats renderStats = SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
    snprintf(lines[lineCount], sizeof(lines[lineCount]), "Main: %u draws, %llu tris  Shadows: %u draws, %llu tris", renderStats.mainDraws,
             (unsigned long long)renderStats.mainTriangles, renderStats.shadowDraws, (unsigned long long)renderStats.shadowTriangles);
    lineCount++;
    snprintf(lines[lineCount], sizeof(lines[lineCount]), "Instanced: %u draws (saved %u)  Visible: %u  Culled: %u", renderStats.instancedDraws,
             renderStats.batchedInstances > renderStats.instancedDraws ? renderStats.batchedInstances - renderStats.instancedDraws : 0u,
             renderStats.visible, renderStats.culled);
    lineCount++;
    // the frame's CPU time, and its GPU time while something times the passes (the Profiler)
    const FrameTimeSample frameTime = FrameTimes::latest();
    if (frameTime.frame > 0) {
        float gpuMs = -1.0f;
        for (size_t i = FrameTimes::size(); i-- > 0 && FrameTimes::size() - i <= 8;) {
            if (FrameTimes::at(i).gpuMs >= 0.0f) {
                gpuMs = FrameTimes::at(i).gpuMs;
                break;
            }
        }
        if (gpuMs >= 0.0f)
            snprintf(lines[lineCount], sizeof(lines[lineCount]), "CPU %.2f ms  GPU %.2f ms", frameTime.cpuMs, gpuMs);
        else
            snprintf(lines[lineCount], sizeof(lines[lineCount]), "CPU %.2f ms", frameTime.cpuMs);
        lineCount++;
    }
    if (const DeviceProfile* device = Platform::getSimulation().device) {
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "Device: %s, %s", device->name,
                 PlatformRules::orientationName(Platform::getSimulation().orientation));
        lineCount++;
    }
    // the Device Tier in effect and the thermal response (S56)
    {
        const char* tier = Device::isTierApplied() ? DeviceNames::tierLabel(Device::getTier()) : "Auto (none applied)";
        const ThermalState response = QualityResolver::getThermalResponse();
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "Device tier: %s (%s)  Thermal: %s%s", tier,
                 DeviceNames::groupLabel(QualityResolver::getPlatformGroup()), DeviceNames::thermalLabel(Device::getThermalState()),
                 response >= ThermalState::Serious ? ", responding" : "");
        lineCount++;
    }
    // Screen.safeArea, canvas units
    const Rect safe = Screen::getSafeArea();
    snprintf(lines[lineCount], sizeof(lines[lineCount]), "Safe area: %g,%g %gx%g", std::round(safe.getX()), std::round(safe.getY()),
             std::round(safe.getWidth()), std::round(safe.getHeight()));
    lineCount++;
    // Dynamic Batching (Unity's Stats: Batches, Saved by batching) of the scene's last frame in the
    // view the game draws
    if (render){
        // (the game's own frame while it plays, else the Game view's)
        const RenderSystem::BatchFrame* batching = render->getBatchFrame(playing ? SystemRender::FRAME_STATS_FRAME : GAME_VIEW_ID);
        if (batching && batching->stats.enabled){
            snprintf(lines[lineCount], sizeof(lines[lineCount]), "Batches: %u   Saved by batching: %u", batching->stats.batches,
                     batching->stats.savedByBatching());
            lineCount++;
        }
    }
    // the scene's HTML UI documents (their offscreen passes are not in the counters above)
    UIDocumentSystem* documents = (sceneProject && sceneProject->scene) ? sceneProject->scene->getSystem<UIDocumentSystem>().get() : nullptr;
    if (documents && documents->hasDocuments()) {
        const HtmlUIRenderStats& ui = documents->getRenderStats();
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "HTML UI: %u passes, %u draws (%u merged), %u instances", ui.passes,
                 ui.drawCalls, ui.runsMerged, ui.instancesDrawn);
        lineCount++;
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "HTML UI: %u stencil draws, glyph atlas %.1f MiB", ui.stencilDraws,
                 (double)ui.atlasBytes / (1024.0 * 1024.0));
        lineCount++;
    }

    // S48: the game frozen by a hit-stop, and for how long still
    if (playing && ::jasperengine::pixelart::HitStop::isActive() && lineCount < 16) {
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "HIT STOP %.2f s", ::jasperengine::pixelart::HitStop::getRemaining());
        lineCount++;
    }

    std::vector<std::string> all(lines, lines + lineCount);
    // after "Draw calls: ... Tris: ..."
    all.insert(all.begin() + 3, meshLines.begin(), meshLines.end());
    drawStatsBox(ImGui::GetWindowDrawList(), imageMin, imageMax, all);
}
