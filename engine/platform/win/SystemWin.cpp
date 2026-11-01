// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemWin.h"

#include "HostServices.h"
#include "JasperEngineWin.h"
#include "WinInputRouter.h"
#include "WindowWin.h"

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#elif defined(SOKOL_D3D11)
#include "D3D11Context.h"
#endif

#include "io/FileUtils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>

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

// An export (the Exporter defines JASPERENGINE_APP_IDENTIFIER) saves per user, as Program Files
// is not writable: %APPDATA%\<identifier>. Other builds (a project run from outside the editor,
// engine tests) keep the working directory.
std::string SystemWin::getUserDataPath() {
#ifdef JASPERENGINE_APP_IDENTIFIER
    static const std::string path = []() {
        wchar_t* appData = nullptr;
        size_t appDataLength = 0;
        std::string base;
        if (_wdupenv_s(&appData, &appDataLength, L"APPDATA") == 0 && appData && appData[0])
            base = winWideToUtf8(appData, static_cast<int>(wcslen(appData)));
        free(appData);
        if (base.empty()) return std::string(".");
        const std::string dir = base + "\\" + JASPERENGINE_APP_IDENTIFIER;
        return FileUtils::prepareUserDataFolder(dir) ? dir : std::string(".");
    }();
    return path;
#else
    return ".";
#endif
}

// Engine paths are UTF-8; fopen would read them in the ANSI code page (a user folder named
// "José" would not open)
FILE* SystemWin::platformFopen(const char* fname, const char* mode) {
    if (!fname || !mode) return nullptr;
    const int length = static_cast<int>(std::strlen(fname));
    const int size = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fname, length, nullptr, 0) : 0;
    if (size <= 0) return std::fopen(fname, mode);
    std::wstring wideName(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, fname, length, &wideName[0], size);
    const std::wstring wideMode(mode, mode + std::strlen(mode));
    // _wfsopen: shared like fopen (_wfopen_s would lock the file)
    return _wfsopen(wideName.c_str(), wideMode.c_str(), _SH_DENYNO);
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

bool SystemWin::platformOpenURL(const std::string& url) {
    return HostServices::openURL(url);
}

std::vector<std::string> SystemWin::getPreferredLocales() {
    std::vector<std::string> locales = HostServices::preferredLocales();
    return locales.empty() ? System::getPreferredLocales() : locales;
}

std::string SystemWin::getRegionOverride() {
    return HostServices::regionOverride();
}

std::string SystemWin::getOperatingSystem() {
    static const std::string value = HostServices::operatingSystem();
    return value;
}

std::string SystemWin::getOperatingSystemVersion() {
    static const std::string value = HostServices::operatingSystemVersion();
    return value;
}

std::string SystemWin::getDeviceModel() {
    static const std::string value = HostServices::deviceModel();
    return value;
}

DeviceType SystemWin::getDeviceType() {
    return DeviceType::Desktop;
}

int SystemWin::getSystemMemorySize() {
    static const int value = HostServices::systemMemoryMiB();
    return value;
}

NetworkReachability SystemWin::getInternetReachability() {
    return HostServices::internetReachability();
}

float SystemWin::getScreenDpi() {
    return HostServices::screenDpi();
}

float SystemWin::getRefreshRate() {
    return HostServices::refreshRate();
}

void SystemWin::setKeepScreenOn(bool keepOn) {
    HostServices::setKeepScreenOn(keepOn);
}

void SystemWin::applyFramePacing(const FramePacing& pacing) {
    JasperEngineWin::applySwapInterval(pacing.swapInterval);
}

int SystemWin::getMaxSwapInterval() {
#if defined(SOKOL_VULKAN)
    return 1;
#else
    // wglSwapIntervalEXT and IDXGISwapChain::Present take an interval up to 4
    return 4;
#endif
}
