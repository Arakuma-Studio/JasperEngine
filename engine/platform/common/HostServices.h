// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// The platform services of a desktop host (System.openURL, the locale, device information):
// one implementation per desktop OS (platform/linux/HostServicesLinux.cpp,
// platform/win/HostServicesWin.cpp, platform/mac/HostServicesMac.mm), shared by the
// desktop System implementations and the editor's Platform, so Play and an exported desktop
// build report the same values.

#ifndef HostServices_h
#define HostServices_h

#include "System.h"

#include <string>
#include <vector>

namespace jasperengine {
namespace HostServices {

    // Hands an already checked URL (System::openURL) to the desktop's handler without a
    // shell: xdg-open, ShellExecuteW, NSWorkspace. True when the handler was started.
    bool openURL(const std::string& url);

    // BCP 47 tags, most preferred first (may be empty: the caller falls back)
    std::vector<std::string> preferredLocales();
    // the region set apart from the language, "" when the host has none
    std::string regionOverride();

    // Unity's SystemInfo.operatingSystem format and the version alone
    std::string operatingSystem();
    std::string operatingSystemVersion();
    // the machine's product name, "PC" when unknown
    std::string deviceModel();
    // MiB, 0 when unknown
    int systemMemoryMiB();
    NetworkReachability internetReachability();

    // of the screen the game (or the editor) window is on; 0 / 60 when unknown
    float screenDpi();
    float refreshRate();

    // keeps the display from sleeping while on (no-op where the desktop has no such request)
    void setKeepScreenOn(bool keepOn);

}
}

#endif /* HostServices_h */
