// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HostServices.h"

#include "util/PlatformRules.h"

// GetUserPreferredUILanguages, GetUserDefaultLocaleName, RegGetValueW (Vista and later)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <netlistmgr.h>

#include <cwchar>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    std::wstring toWide(const std::string& utf8){
        if (utf8.empty()) return std::wstring();
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (count <= 0) return std::wstring();
        std::wstring wide((size_t)count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), count);
        return wide;
    }

    std::string toUtf8(const wchar_t* text, int length = -1){
        if (!text) return std::string();
        const int count = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        if (count <= 0) return std::string();
        std::string out((size_t)count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), count, nullptr, nullptr);
        if (length < 0 && !out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }

    // RtlGetVersion tells the real version (GetVersionEx lies to unmanifested processes)
    bool realVersion(RTL_OSVERSIONINFOW& info){
        using RtlGetVersionProc = LONG (WINAPI*)(RTL_OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionProc>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion) return false;
        ZeroMemory(&info, sizeof(info));
        info.dwOSVersionInfoSize = sizeof(info);
        return rtlGetVersion(&info) == 0;
    }

    bool is64BitWindows(){
#if defined(_WIN64)
        return true;
#else
        BOOL wow64 = FALSE;
        return IsWow64Process(GetCurrentProcess(), &wow64) && wow64;
#endif
    }

    // the window the game or the editor draws in (the foreground one is ours while it plays)
    HWND currentWindow(){
        HWND window = GetActiveWindow();
        return window ? window : GetForegroundWindow();
    }

}

bool HostServices::openURL(const std::string& url){
    const std::wstring wide = toWide(url);
    if (wide.empty()) return false;
    // above 32 is success (ShellExecute's old HINSTANCE contract)
    const INT_PTR result = (INT_PTR)ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return result > 32;
}

std::vector<std::string> HostServices::preferredLocales(){
    std::vector<std::string> out;
    ULONG count = 0;
    ULONG size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0){
        std::vector<wchar_t> buffer(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)){
            // a double-null-terminated list
            for (const wchar_t* name = buffer.data(); *name; name += wcslen(name) + 1){
                const std::string tag = LocaleRules::normalize(toUtf8(name));
                if (!tag.empty()) out.push_back(tag);
            }
        }
    }
    if (out.empty()){
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0){
            const std::string tag = LocaleRules::normalize(toUtf8(name));
            if (!tag.empty()) out.push_back(tag);
        }
    }
    return out;
}

std::string HostServices::regionOverride(){
    // the home location (Settings > Time & language > Region), Windows 10 1709+
    using GetUserDefaultGeoNameProc = int (WINAPI*)(LPWSTR, int);
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")){
        auto getGeoName = reinterpret_cast<GetUserDefaultGeoNameProc>(GetProcAddress(kernel, "GetUserDefaultGeoName"));
        if (getGeoName){
            wchar_t geo[16] = {0};
            if (getGeoName(geo, 16) > 0){
                const std::string region = toUtf8(geo);
                // two letters; a UN M.49 code ("001", the world) is no country
                if (region.size() == 2) return region;
            }
        }
    }
    // else the region of the user's locale name ("es-MX")
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0){
        return LocaleRules::regionCode(toUtf8(name));
    }
    return "";
}

std::string HostServices::operatingSystem(){
    RTL_OSVERSIONINFOW info;
    if (!realVersion(info)) return "Windows";
    return OsRules::windowsOperatingSystem(info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber, is64BitWindows());
}

std::string HostServices::operatingSystemVersion(){
    RTL_OSVERSIONINFOW info;
    if (!realVersion(info)) return "";
    return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
}

std::string HostServices::deviceModel(){
    wchar_t value[256] = {0};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName",
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS){
        std::string product = toUtf8(value);
        while (!product.empty() && product.back() == ' ') product.pop_back();
        if (!product.empty() && product != "To Be Filled By O.E.M." && product != "System Product Name" &&
            product != "Default string"){
            return product;
        }
    }
    return "PC";
}

int HostServices::systemMemoryMiB(){
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return (int)(status.ullTotalPhys / (1024ULL * 1024ULL));
}

NetworkReachability HostServices::internetReachability(){
    // COM may already be up in another mode on this thread: then it is used as it is
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);
    NetworkReachability reachability = NetworkReachability::NotReachable;
    INetworkListManager* manager = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager,
                                   reinterpret_cast<void**>(&manager))) && manager){
        NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
        if (SUCCEEDED(manager->GetConnectivity(&connectivity))){
            const int connected = NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET |
                                  NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK |
                                  NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET;
            if (connectivity & connected) reachability = NetworkReachability::ReachableViaLocalAreaNetwork;
        }
        manager->Release();
    }
    if (uninitialize) CoUninitialize();
    return reachability;
}

float HostServices::screenDpi(){
    using GetDpiForWindowProc = UINT (WINAPI*)(HWND);
    using GetDpiForSystemProc = UINT (WINAPI*)();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")){
        HWND window = currentWindow();
        auto forWindow = reinterpret_cast<GetDpiForWindowProc>(GetProcAddress(user32, "GetDpiForWindow"));
        if (forWindow && window){
            const UINT dpi = forWindow(window);
            if (dpi > 0) return (float)dpi;
        }
        auto forSystem = reinterpret_cast<GetDpiForSystemProc>(GetProcAddress(user32, "GetDpiForSystem"));
        if (forSystem){
            const UINT dpi = forSystem();
            if (dpi > 0) return (float)dpi;
        }
    }
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? (float)dpi : 96.0f;
}

float HostServices::refreshRate(){
    // the monitor of the window, else the primary one
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    HWND window = currentWindow();
    const wchar_t* device = nullptr;
    if (window && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)){
        device = monitor.szDevice;
    }
    if (EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1){
        return (float)mode.dmDisplayFrequency;
    }
    return 60.0f;
}

void HostServices::setKeepScreenOn(bool keepOn){
    SetThreadExecutionState(keepOn ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
}
