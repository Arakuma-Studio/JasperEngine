// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemLinux.h"

#include "LinuxInputRouter.h"
#include "WindowLinux.h"
#include "io/FileUtils.h"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

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

// An export (the Exporter defines JASPERENGINE_APP_IDENTIFIER) saves per user, as the install
// folder may not be writable: $XDG_DATA_HOME/<identifier>, by default ~/.local/share/<identifier>.
// Other builds (a project run from outside the editor, engine tests) keep the working directory.
std::string SystemLinux::getUserDataPath() {
#ifdef JASPERENGINE_APP_IDENTIFIER
    static const std::string path = []() {
        std::string base;
        // the XDG spec: a relative value is invalid and ignored
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg && xdg[0] == '/') {
            base = xdg;
        } else {
            const char* home = std::getenv("HOME");
            if (!home || !home[0]) {
                const passwd* user = getpwuid(getuid());
                home = user ? user->pw_dir : nullptr;
            }
            if (home && home[0]) base = std::string(home) + "/.local/share";
        }
        if (base.empty()) return std::string(".");
        const std::string dir = base + "/" + JASPERENGINE_APP_IDENTIFIER;
        return FileUtils::prepareUserDataFolder(dir) ? dir : std::string(".");
    }();
    return path;
#else
    return ".";
#endif
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
