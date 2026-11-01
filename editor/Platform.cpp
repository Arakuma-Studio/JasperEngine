// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Platform.h"
#include "Out.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Engine.h"
#include "HostServices.h"
#include "mcp/McpServer.h"
#include "util/PlatformRules.h"
#include "window/DeviceProfiles.h"
#include "imgui.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace jasperengine;

int editor::Platform::width = 0;
int editor::Platform::height = 0;

editor::PlatformSimulation editor::Platform::simulation;
ScreenOrientation editor::Platform::shownOrientation = ScreenOrientation::Portrait;
ScreenOrientation editor::Platform::settingsOrientation = ScreenOrientation::Portrait;
ScreenOrientation editor::Platform::requestedOrientation = ScreenOrientation::AutoRotation;
uint8_t editor::Platform::allowedMask = PlatformRules::ALL_ORIENTATIONS;
bool editor::Platform::warnedUnsupportedOrientation = false;
std::deque<editor::OpenedUrl> editor::Platform::openedUrls;
int editor::Platform::openedUrlCount = 0;
int editor::Platform::vibrationCount = 0;
int editor::Platform::hapticCount = 0;
bool editor::Platform::hostFocused = true;
bool editor::Platform::hostFocusKnown = false;

namespace {
    // get_play_state.platform.opened_urls keeps the last ones
    constexpr size_t MAX_OPENED_URLS = 10;
}

editor::Platform::Platform(Project* project) : System() {
    this->project = project;
}

bool editor::Platform::setSizes(int width, int height){
    if (editor::Platform::width != width || editor::Platform::height != height){
        editor::Platform::width = width;
        editor::Platform::height = height;

        return true;
    }

    return false;
}

int editor::Platform::getScreenWidth(){
    return width;
}

int editor::Platform::getScreenHeight(){
    return height;
}

std::string editor::Platform::getAssetPath(){
    return project->getAssetsPath().string();
}

std::string editor::Platform::getLuaPath(){
    return project->getLuaPath().string();
}

std::string editor::Platform::getShaderPath(){
    // Compiled shaders are a build output, not a referenced asset
    return (project->getProjectPath() / "shaders").string();
}

std::string editor::Platform::getUserDataPath(){
    return project->getUserDataPath().string();
}

std::string editor::Platform::getImportPath(){
    return project->getImportPath().string();
}

sg_environment editor::Platform::getSokolEnvironment(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolEnvironment();
    #else
    return System::getSokolEnvironment();
    #endif
}

sg_swapchain editor::Platform::getSokolSwapchain(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolSwapchain();
    #else
    return System::getSokolSwapchain();
    #endif
}

bool editor::Platform::isTouchDevice(){
    return project->isSimulateTouch();
}

void editor::Platform::setClipboardText(const std::string& utf8){
    if (ImGui::GetCurrentContext()){
        ImGui::SetClipboardText(utf8.c_str());
    }else{
        System::setClipboardText(utf8);
    }
}

std::string editor::Platform::getClipboardText(){
    if (ImGui::GetCurrentContext()){
        const char* text = ImGui::GetClipboardText();
        return text ? std::string(text) : std::string();
    }
    return System::getClipboardText();
}

void editor::Platform::setMouseMode(MouseMode mode){
    Backend::setMouseMode(mode);
}

void editor::Platform::quit(){
    // stops play instead of closing the editor, deferred to not stop inside a script call
    editor::getEditorHost().enqueueMainThreadTask([this]() {
        project->stopActivePlay();
    });
}

// ------------------------------------------------------------------ device simulation

void editor::Platform::setSimulatedDevice(const DeviceProfile* device, ScreenOrientation orientation){
    if (!PlatformRules::isFixedOrientation(orientation)) orientation = ScreenOrientation::Portrait;
    if (device && !(profileOrientationMask(*device) & PlatformRules::orientationBit(orientation))){
        orientation = ScreenOrientation::Portrait;
    }
    // another device shows its orientation right away; the same one turns with the next layout
    if (simulation.device != device || !device) shownOrientation = orientation;
    settingsOrientation = orientation;
    simulation.device = device;
    simulation.orientation = orientation;
}

void editor::Platform::setSimulatedOrientation(ScreenOrientation orientation){
    if (!PlatformRules::isFixedOrientation(orientation)) return;
    if (simulation.device && !(profileOrientationMask(*simulation.device) & PlatformRules::orientationBit(orientation))) return;
    simulation.orientation = orientation;
}

void editor::Platform::setSimulatedLanguage(const std::string& language){
    simulation.language = LocaleRules::normalize(language);
}

void editor::Platform::setSimulatedDpi(float dpi){
    simulation.dpi = (std::isfinite(dpi) && dpi > 0.0f) ? dpi : 0.0f;
}

const editor::PlatformSimulation& editor::Platform::getSimulation(){
    return simulation;
}

void editor::Platform::setShownOrientation(ScreenOrientation orientation){
    if (PlatformRules::isFixedOrientation(orientation)) shownOrientation = orientation;
}

ScreenOrientation editor::Platform::getRequestedOrientation(){
    return requestedOrientation;
}

uint8_t editor::Platform::getAllowedMask(){
    return allowedMask;
}

void editor::Platform::resetPlayRecords(){
    openedUrls.clear();
    openedUrlCount = 0;
    vibrationCount = 0;
    hapticCount = 0;
    warnedUnsupportedOrientation = false;
    // Play starts focused: only later changes of the window's focus count
    hostFocusKnown = false;
}

void editor::Platform::endPlaySimulation(){
    requestedOrientation = ScreenOrientation::AutoRotation;
    allowedMask = PlatformRules::ALL_ORIENTATIONS;
    simulation.orientation = settingsOrientation;
    hostFocusKnown = false;
}

const std::deque<editor::OpenedUrl>& editor::Platform::getOpenedUrls(){
    return openedUrls;
}

int editor::Platform::getOpenedUrlCount(){
    return openedUrlCount;
}

int editor::Platform::getVibrationCount(){
    return vibrationCount;
}

int editor::Platform::getHapticCount(){
    return hapticCount;
}

void editor::Platform::forwardHostFocus(bool focused, bool playSessionActive){
    if (!playSessionActive || isHeadless()){
        hostFocusKnown = false;
        return;
    }
    if (!hostFocusKnown){
        hostFocusKnown = true;
        hostFocused = focused;
        return;
    }
    if (focused == hostFocused) return;
    hostFocused = focused;
    Engine::systemFocus(focused);
}

bool editor::Platform::isHeadless(){
    if (mcp::mcpLaunchOptions().headless) return true;
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((!display || !display[0]) && (!wayland || !wayland[0])) return true;
#endif
    return false;
}

// ------------------------------------------------------------------ platform services

bool editor::Platform::platformOpenURL(const std::string& url){
    OpenedUrl entry;
    entry.url = url;
    entry.time = Engine::getSystemTime();
    entry.launched = !isHeadless() && HostServices::openURL(url);
    openedUrls.push_back(entry);
    while (openedUrls.size() > MAX_OPENED_URLS) openedUrls.pop_front();
    openedUrlCount++;
    if (entry.launched){
        editor::Out::info("Open URL: %s", url.c_str());
    }else if (isHeadless()){
        editor::Out::info("Open URL: %s (recorded; no browser in a headless run)", url.c_str());
    }else{
        editor::Out::warning("Open URL: %s (the system has no handler for it)", url.c_str());
    }
    // a headless run answers as the device would: the request was taken
    return entry.launched || isHeadless();
}

std::vector<std::string> editor::Platform::getPreferredLocales(){
    if (!simulation.language.empty()) return {simulation.language};
    std::vector<std::string> locales = HostServices::preferredLocales();
    return locales.empty() ? System::getPreferredLocales() : locales;
}

std::string editor::Platform::getRegionOverride(){
    // a simulated language carries its region
    if (!simulation.language.empty()) return "";
    return HostServices::regionOverride();
}

std::string editor::Platform::getAppVersion(){
    return project->getApplicationVersion();
}

int64_t editor::Platform::getAppBuild(){
    return (int64_t)project->getApplicationVersionCode();
}

std::string editor::Platform::getAppIdentifier(){
    return project->getApplicationIdentifier();
}

std::string editor::Platform::getAppName(){
    return project->getApplicationName();
}

std::string editor::Platform::getOperatingSystem(){
    static const std::string value = HostServices::operatingSystem();
    return value;
}

std::string editor::Platform::getOperatingSystemVersion(){
    static const std::string value = HostServices::operatingSystemVersion();
    return value;
}

std::string editor::Platform::getDeviceModel(){
    if (simulation.device) return simulation.device->model;
    static const std::string value = HostServices::deviceModel();
    return value;
}

DeviceType editor::Platform::getDeviceType(){
    return simulation.device ? DeviceType::Handheld : DeviceType::Desktop;
}

int editor::Platform::getSystemMemorySize(){
    static const int value = HostServices::systemMemoryMiB();
    return value;
}

bool editor::Platform::isEditor(){
    return true;
}

NetworkReachability editor::Platform::getInternetReachability(){
    return HostServices::internetReachability();
}

bool editor::Platform::vibrate(int milliseconds){
    vibrationCount++;
    return false;
}

bool editor::Platform::hapticFeedback(HapticFeedback type){
    hapticCount++;
    return false;
}

float editor::Platform::getScreenDpi(){
    if (simulation.device && simulation.device->dpi > 0.0f) return simulation.device->dpi;
    if (simulation.dpi > 0.0f) return simulation.dpi;
    return HostServices::screenDpi();
}

float editor::Platform::getRefreshRate(){
    return HostServices::refreshRate();
}

void editor::Platform::applyOrientationPolicy(ScreenOrientation requested, uint8_t mask){
    requestedOrientation = requested;
    allowedMask = mask;
    if (!simulation.device) return;

    // Unity's Device Simulator: a request the simulated device's orientation does not fit
    // turns the device to one that does
    const uint8_t deviceMask = profileOrientationMask(*simulation.device);
    const uint8_t wanted = PlatformRules::effectiveMask(requested, mask) & deviceMask;
    if (wanted == 0){
        if (!warnedUnsupportedOrientation){
            warnedUnsupportedOrientation = true;
            editor::Out::warning("Game view: the simulated %s cannot show %s; it stays as it is",
                                 simulation.device->name, PlatformRules::orientationName(requested));
        }
        return;
    }
    const ScreenOrientation target = PlatformRules::resolveOrientation(simulation.orientation, ScreenOrientation::AutoRotation, wanted);
    if (target != simulation.orientation){
        simulation.orientation = target;
    }
}

ScreenOrientation editor::Platform::getScreenOrientation(){
    if (simulation.device) return shownOrientation;
    return System::getScreenOrientation();
}

DeviceOrientation editor::Platform::getDeviceOrientation(){
    if (!simulation.device) return DeviceOrientation::Unknown;
    return PlatformRules::deviceOrientationOf(simulation.orientation);
}

void editor::Platform::platformLog(const int type, const char *fmt, va_list args){
    // Sized to the message: a Lua print or a traceback can be longer than any fixed buffer
    char stackBuf[4096];
    va_list sizeArgs;
    va_copy(sizeArgs, args);
    const int size = vsnprintf(stackBuf, sizeof(stackBuf), fmt, sizeArgs);
    va_end(sizeArgs);
    std::string heapBuf;
    const char* buf = stackBuf;
    if (size >= static_cast<int>(sizeof(stackBuf))) {
        heapBuf.resize(static_cast<size_t>(size) + 1);
        vsnprintf(&heapBuf[0], heapBuf.size(), fmt, args);
        heapBuf.resize(static_cast<size_t>(size));
        buf = heapBuf.c_str();
    } else if (size < 0) {
        stackBuf[0] = '\0';
    }

    switch (type) {
        case D_LOG_WARN:
            editor::Out::warning(buf);
            break;
        case D_LOG_ERROR:
            editor::Out::error(buf);
            break;
        default:
            editor::Out::info(buf);
            break;
    }
}
