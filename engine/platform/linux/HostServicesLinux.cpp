// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HostServices.h"

#include "WindowLinux.h"
#include "util/PlatformRules.h"

#include <X11/Xlib.h>
#include <X11/Xresource.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

extern char** environ;

using namespace jasperengine;

namespace {

    std::string trim(std::string value){
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
        size_t start = 0;
        while (start < value.size() && std::isspace((unsigned char)value[start])) start++;
        return value.substr(start);
    }

    std::string readFirstLine(const char* path){
        std::ifstream file(path);
        std::string line;
        if (file) std::getline(file, line);
        return trim(line);
    }

    // a KEY="value" line of /etc/os-release (or /usr/lib/os-release)
    std::string osReleaseValue(const std::string& key){
        for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}){
            std::ifstream file(path);
            if (!file) continue;
            std::string line;
            while (std::getline(file, line)){
                if (line.compare(0, key.size() + 1, key + "=") != 0) continue;
                std::string value = trim(line.substr(key.size() + 1));
                if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()){
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            }
            return "";
        }
        return "";
    }

    std::string kernelRelease(){
        utsname name{};
        if (uname(&name) != 0) return "";
        return name.release;
    }

    const char* env(const char* name){
        const char* value = std::getenv(name);
        return (value && value[0]) ? value : nullptr;
    }

}

bool HostServices::openURL(const std::string& url){
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    // the handler's chatter stays out of the game's output
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::string program = "xdg-open";
    std::string argument = url;
    char* argv[] = {program.data(), argument.data(), nullptr};
    pid_t pid = 0;
    const int result = posix_spawnp(&pid, "xdg-open", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) return false;

    // reaped apart, so the game never waits for the browser
    std::thread([pid](){
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR){}
    }).detach();
    return true;
}

std::vector<std::string> HostServices::preferredLocales(){
    std::vector<std::string> out;
    // the C locale turns LANGUAGE off, as gettext does
    const char* current = env("LC_ALL");
    if (!current) current = env("LC_MESSAGES");
    if (!current) current = env("LANG");
    const std::string normalizedCurrent = current ? LocaleRules::normalize(current) : std::string();
    const bool cLocale = current && (std::strcmp(current, "C") == 0 || std::strcmp(current, "POSIX") == 0);

    if (const char* list = env("LANGUAGE")){
        if (!cLocale){
            out = LocaleRules::parseLanguageList(list);
        }
    }
    if (!normalizedCurrent.empty() && std::find(out.begin(), out.end(), normalizedCurrent) == out.end()){
        out.push_back(normalizedCurrent);
    }
    return out;
}

std::string HostServices::regionOverride(){
    return "";
}

std::string HostServices::operatingSystem(){
    std::string text = "Linux " + kernelRelease();
    const std::string pretty = osReleaseValue("PRETTY_NAME");
    if (!pretty.empty()) text += " " + pretty;
    return text;
}

std::string HostServices::operatingSystemVersion(){
    const std::string version = osReleaseValue("VERSION_ID");
    return version.empty() ? kernelRelease() : version;
}

std::string HostServices::deviceModel(){
    std::string product = readFirstLine("/sys/devices/virtual/dmi/id/product_name");
    // firmware placeholders say nothing about the machine
    if (product.empty() || product == "To Be Filled By O.E.M." || product == "System Product Name" ||
        product == "Default string"){
        return "PC";
    }
    return product;
}

int HostServices::systemMemoryMiB(){
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) return 0;
    return (int)(((long long)pages * (long long)pageSize) / (1024LL * 1024LL));
}

NetworkReachability HostServices::internetReachability(){
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return NetworkReachability::NotReachable;
    bool reachable = false;
    for (ifaddrs* it = addresses; it && !reachable; it = it->ifa_next){
        if (!it->ifa_addr) continue;
        const unsigned flags = it->ifa_flags;
        if (!(flags & IFF_UP) || !(flags & IFF_RUNNING) || (flags & IFF_LOOPBACK)) continue;
        const int family = it->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) reachable = true;
    }
    freeifaddrs(addresses);
    return reachable ? NetworkReachability::ReachableViaLocalAreaNetwork : NetworkReachability::NotReachable;
}

float HostServices::screenDpi(){
    Display* display = WindowLinux::display();
    if (!display) return 96.0f;
    // the desktop's font dpi (what GNOME, KDE and xrdb set) is what users scale by
    XrmInitialize();
    if (const char* resources = XResourceManagerString(display)){
        XrmDatabase database = XrmGetStringDatabase(resources);
        if (database){
            char* type = nullptr;
            XrmValue value{};
            float dpi = 0.0f;
            if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr){
                dpi = (float)std::atof(value.addr);
            }
            XrmDestroyDatabase(database);
            if (dpi > 0.0f) return dpi;
        }
    }
    const int screen = DefaultScreen(display);
    const int widthMM = DisplayWidthMM(display, screen);
    if (widthMM > 0){
        const float dpi = (float)DisplayWidth(display, screen) * 25.4f / (float)widthMM;
        if (dpi > 20.0f) return dpi;
    }
    return 96.0f;
}

float HostServices::refreshRate(){
    static float cached = 0.0f;
    if (cached > 0.0f) return cached;
    // asked before the window exists (init()): the usual rate, not cached
    Display* display = WindowLinux::display();
    if (!display) return 60.0f;
    cached = 60.0f;

    // XRandR is loaded when there, so a game does not need it to start
    void* library = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!library) return cached;
    using GetScreenInfo = void* (*)(Display*, Window);
    using CurrentRate = short (*)(void*);
    using FreeInfo = void (*)(void*);
    auto getInfo = reinterpret_cast<GetScreenInfo>(dlsym(library, "XRRGetScreenInfo"));
    auto currentRate = reinterpret_cast<CurrentRate>(dlsym(library, "XRRConfigCurrentRate"));
    auto freeInfo = reinterpret_cast<FreeInfo>(dlsym(library, "XRRFreeScreenConfigInfo"));
    if (getInfo && currentRate && freeInfo){
        if (void* info = getInfo(display, RootWindow(display, DefaultScreen(display)))){
            const short rate = currentRate(info);
            if (rate > 0) cached = (float)rate;
            freeInfo(info);
        }
    }
    dlclose(library);
    return cached;
}

void HostServices::setKeepScreenOn(bool keepOn){
    // X11 has no per-window request (screensavers take inhibit requests over D-Bus)
    (void)keepOn;
}
