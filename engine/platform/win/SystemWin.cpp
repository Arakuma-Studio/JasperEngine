// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemWin.h"

#include "WinInputRouter.h"
#include "WindowWin.h"

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#elif defined(SOKOL_D3D11)
#include "D3D11Context.h"
#endif

#include <cstring>

using namespace jasperengine;

std::string jasperengine::winWideToUtf8(const wchar_t* text, int length) {
    if (!text || length <= 0) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), count,
                        nullptr, nullptr);
    return result;
}

SystemWin::SystemWin(WinInputRouter* router) : router(router) {
}

int SystemWin::getScreenWidth() {
    return WindowWin::getClientWidth();
}

int SystemWin::getScreenHeight() {
    return WindowWin::getClientHeight();
}

#if defined(SOKOL_VULKAN)
sg_environment SystemWin::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemWin::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#elif defined(SOKOL_D3D11)
sg_environment SystemWin::getSokolEnvironment() {
    return D3D11Context::environment();
}

sg_swapchain SystemWin::getSokolSwapchain() {
    return D3D11Context::swapchain();
}
#endif

bool SystemWin::isFullscreen() {
    return WindowWin::isFullscreen();
}

void SystemWin::requestFullscreen() {
    WindowWin::requestFullscreen();
}

void SystemWin::exitFullscreen() {
    WindowWin::exitFullscreen();
}

bool SystemWin::isWindowMaximized() {
    return WindowWin::isMaximized();
}

void SystemWin::maximizeWindow() {
    WindowWin::maximize();
}

void SystemWin::restoreWindow() {
    WindowWin::restore();
}

void SystemWin::setWindowSize(int width, int height) {
    WindowWin::setSize(width, height);
}

bool SystemWin::isWindowResizable() {
    return WindowWin::isResizable();
}

void SystemWin::setWindowResizable(bool resizable) {
    WindowWin::setResizable(resizable);
}

void SystemWin::setWindowTitle(const std::string& title) {
    WindowWin::setTitle(title);
}

void SystemWin::quit() {
    WindowWin::quit();
}

void SystemWin::setMouseCursor(CursorType type) {
    WindowWin::setMouseCursor(type);
}

void SystemWin::setMouseMode(MouseMode mode) {
    WindowWin::setMouseMode(mode);
}

void SystemWin::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowWin::setMousePosition(x, y);
}

void SystemWin::hideVirtualKeyboard() {
    if (router) router->cancelComposition();
    System::hideVirtualKeyboard();
}

void SystemWin::setClipboardText(const std::string& utf8) {
    // Without an owner window EmptyClipboard leaves the clipboard ownerless,
    // and SetClipboardData then fails.
    HWND window = WindowWin::handle();
    if (!window) return;

    // Filled before opening, so the clipboard is held for as short as possible
    const std::wstring text = winUtf8ToWide(utf8);
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);

    // Another application may have it open; the copy is dropped then
    if (!OpenClipboard(window)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    // Once SetClipboardData succeeds the memory belongs to the system
    if (!SetClipboardData(CF_UNICODETEXT, memory))
        GlobalFree(memory);
    CloseClipboard();
}

std::string SystemWin::getClipboardText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
    if (!OpenClipboard(WindowWin::handle())) return {};

    // Copied out first so the clipboard is closed before converting
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            // Bounded by the block in case the writer left out the terminator
            const size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            size_t length = 0;
            while (length < capacity && chars[length] != L'\0') ++length;
            text.assign(chars, length);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();

    return winWideToUtf8(text.data(), static_cast<int>(text.size()));
}

void SystemWin::setTextInputArea(float x, float y, float width, float height) {
    if (router) router->setTextInputArea(x, y, width, height);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemWin::getAssetPath() {
#ifdef JASPERENGINE_ASSET_PATH
    return JASPERENGINE_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemWin::getUserDataPath() {
    return ".";
}

std::string SystemWin::getLuaPath() {
#ifdef JASPERENGINE_LUA_PATH
    return JASPERENGINE_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemWin::getShaderPath() {
#ifdef JASPERENGINE_SHADER_PATH
    return JASPERENGINE_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
