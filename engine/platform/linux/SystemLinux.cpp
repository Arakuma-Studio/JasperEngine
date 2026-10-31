// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemLinux.h"

#include "HostServices.h"
#include "JasperEngineLinux.h"
#include "LinuxInputRouter.h"
#include "WindowLinux.h"

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#endif

using namespace jasperengine;

SystemLinux::SystemLinux(LinuxInputRouter* router) : router(router) {
}

int SystemLinux::getScreenWidth() {
    return WindowLinux::getWidth();
}

int SystemLinux::getScreenHeight() {
    return WindowLinux::getHeight();
}

#if defined(SOKOL_VULKAN)
sg_environment SystemLinux::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemLinux::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#endif

bool SystemLinux::isFullscreen() {
    return WindowLinux::isFullscreen();
}

void SystemLinux::requestFullscreen() {
    WindowLinux::requestFullscreen();
}

void SystemLinux::exitFullscreen() {
    WindowLinux::exitFullscreen();
}

bool SystemLinux::isWindowMaximized() {
    return WindowLinux::isMaximized();
}

void SystemLinux::maximizeWindow() {
    WindowLinux::maximize();
}

void SystemLinux::restoreWindow() {
    WindowLinux::restore();
}

void SystemLinux::setWindowSize(int width, int height) {
    WindowLinux::setSize(width, height);
}

bool SystemLinux::isWindowResizable() {
    return WindowLinux::isResizable();
}

void SystemLinux::setWindowResizable(bool resizable) {
    WindowLinux::setResizable(resizable);
}

void SystemLinux::setWindowTitle(const std::string& title) {
    WindowLinux::setTitle(title);
}

void SystemLinux::quit() {
    WindowLinux::quit();
}

void SystemLinux::setClipboardText(const std::string& utf8) {
    WindowLinux::setClipboardText(utf8);
}

std::string SystemLinux::getClipboardText() {
    return WindowLinux::getClipboardText();
}

void SystemLinux::setTextInputArea(float x, float y, float width, float height) {
    (void)width;
    if (router) router->setTextInputArea(int(x), int(y), int(height));
}

void SystemLinux::setMouseCursor(CursorType type) {
    WindowLinux::setMouseCursor(type);
}

void SystemLinux::setMouseMode(MouseMode mode) {
    WindowLinux::setMouseMode(mode);
}

void SystemLinux::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowLinux::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemLinux::getAssetPath() {
#ifdef JASPERENGINE_ASSET_PATH
    return JASPERENGINE_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemLinux::getUserDataPath() {
    return ".";
}

std::string SystemLinux::getLuaPath() {
#ifdef JASPERENGINE_LUA_PATH
    return JASPERENGINE_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemLinux::getShaderPath() {
#ifdef JASPERENGINE_SHADER_PATH
    return JASPERENGINE_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}

bool SystemLinux::platformOpenURL(const std::string& url) {
    return HostServices::openURL(url);
}

std::vector<std::string> SystemLinux::getPreferredLocales() {
    std::vector<std::string> locales = HostServices::preferredLocales();
    return locales.empty() ? System::getPreferredLocales() : locales;
}

std::string SystemLinux::getOperatingSystem() {
    static const std::string value = HostServices::operatingSystem();
    return value;
}

std::string SystemLinux::getOperatingSystemVersion() {
    static const std::string value = HostServices::operatingSystemVersion();
    return value;
}

std::string SystemLinux::getDeviceModel() {
    static const std::string value = HostServices::deviceModel();
    return value;
}

DeviceType SystemLinux::getDeviceType() {
    return DeviceType::Desktop;
}

int SystemLinux::getSystemMemorySize() {
    static const int value = HostServices::systemMemoryMiB();
    return value;
}

NetworkReachability SystemLinux::getInternetReachability() {
    return HostServices::internetReachability();
}

float SystemLinux::getScreenDpi() {
    return HostServices::screenDpi();
}

float SystemLinux::getRefreshRate() {
    return HostServices::refreshRate();
}

void SystemLinux::applyFramePacing(const FramePacing& pacing) {
    JasperEngineLinux::applySwapInterval(pacing.swapInterval);
}

int SystemLinux::getMaxSwapInterval() {
#if defined(SOKOL_VULKAN)
    return 1;
#else
    // GLX_EXT_swap_control presents every Nth vblank
    return 4;
#endif
}
