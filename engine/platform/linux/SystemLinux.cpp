// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemLinux.h"
#include "PlatformConfig.h"

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
    // a prebuilt player's content root (next to its executable, or --content)
    if (!platformConfig().contentRoot.empty())
        return platformConfig().contentRoot + "/assets";
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
    if (!platformConfig().contentRoot.empty())
        return platformConfig().contentRoot + "/lua";
#ifdef JASPERENGINE_LUA_PATH
    return JASPERENGINE_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemLinux::getShaderPath() {
    if (!platformConfig().contentRoot.empty())
        return System::getShaderPath();
#ifdef JASPERENGINE_SHADER_PATH
    return JASPERENGINE_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
