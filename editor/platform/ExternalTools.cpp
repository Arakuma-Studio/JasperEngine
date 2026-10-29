// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExternalTools.h"

#include "AppSettings.h"

#include <cstdlib>

using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    bool executable(const fs::path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
#ifdef _WIN32
        return true;
#else
        return (fs::status(path, ec).permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
#endif
    }

    std::vector<std::string> candidates(const std::string& name) {
#ifdef _WIN32
        if (fs::path(name).has_extension()) return {name};
        return {name + ".exe", name + ".bat", name + ".cmd", name};
#else
        return {name};
#endif
    }

}

fs::path ExternalTools::findOnPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string list = path;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(separator, start);
        if (end == std::string::npos) end = list.size();
        const std::string dir = list.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;
        for (const std::string& file : candidates(name))
            if (executable(fs::path(dir) / file)) return fs::path(dir) / file;
        if (end == list.size()) break;
    }
    return {};
}

ToolLocation ExternalTools::find(const std::string& name, const std::string& preference, const std::vector<std::string>& envVars,
                                 const std::string& subdir, const std::string& hint) {
    ToolLocation location{name, {}, "", hint};
    auto inDir = [&](const fs::path& dir) -> fs::path {
        for (const std::string& file : candidates(name)) {
            if (executable(dir / file)) return dir / file;
            if (executable(dir / "bin" / file)) return dir / "bin" / file;
        }
        return {};
    };
    if (!preference.empty()) {
        const std::string configured = AppSettings::getExternalToolPath(preference);
        if (!configured.empty()) {
            if (executable(configured)) location.path = configured;
            else location.path = inDir(configured);
            if (!location.path.empty()) {
                location.source = "preferences";
                return location;
            }
        }
    }
    for (const std::string& var : envVars) {
        const char* value = std::getenv(var.c_str());
        if (!value || !*value) continue;
        const fs::path dir = subdir.empty() ? fs::path(value) : fs::path(value) / subdir;
        location.path = inDir(dir);
        if (!location.path.empty()) {
            location.source = var;
            return location;
        }
    }
    location.path = findOnPath(name);
    if (!location.path.empty()) location.source = "PATH";
    return location;
}

ToolLocation ExternalTools::steamcmd() {
    return find("steamcmd", "steamcmd", {"STEAMCMD_HOME"}, "", "SteamCMD (https://developer.valvesoftware.com/wiki/SteamCMD)");
}

ToolLocation ExternalTools::rcodesign() {
    return find("rcodesign", "rcodesign", {}, "", "apple-codesign's rcodesign (https://github.com/indygreg/apple-platform-rs)");
}

ToolLocation ExternalTools::codesign() {
    return find("codesign", "", {}, "", "Xcode's command line tools (macOS)");
}

ToolLocation ExternalTools::xcodebuild() {
    return find("xcodebuild", "", {}, "", "Xcode (macOS)");
}

ToolLocation ExternalTools::objcopy() {
    return find("objcopy", "", {}, "", "binutils");
}

ToolLocation ExternalTools::strip() {
    return find("strip", "", {}, "", "binutils");
}

ToolLocation ExternalTools::ssh() {
    return find("ssh", "ssh", {}, "", "OpenSSH");
}

ToolLocation ExternalTools::scp() {
    return find("scp", "scp", {}, "", "OpenSSH");
}
