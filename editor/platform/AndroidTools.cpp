// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AndroidTools.h"

#include "App.h"
#include "AppSettings.h"
#include "util/CommandRunner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    // "35.0.0" < "36.1.0"
    bool versionLess(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            long x = 0, y = 0;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) x = x * 10 + (a[i++] - '0');
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) y = y * 10 + (b[j++] - '0');
            if (x != y) return x < y;
            while (i < a.size() && !std::isdigit(static_cast<unsigned char>(a[i]))) i++;
            while (j < b.size() && !std::isdigit(static_cast<unsigned char>(b[j]))) j++;
        }
        return false;
    }

    ToolLocation inDir(const fs::path& dir, const std::string& name, const std::string& source, const std::string& hint) {
        ToolLocation location{name, {}, source, hint};
        std::error_code ec;
#ifdef _WIN32
        for (const std::string& file : {name + ".exe", name + ".bat", name}) {
#else
        for (const std::string& file : {name}) {
#endif
            if (fs::is_regular_file(dir / file, ec)) {
                location.path = dir / file;
                return location;
            }
        }
        return location;
    }

}

fs::path AndroidTools::sdkRoot() {
    const std::string configured = AppSettings::getExternalToolPath("androidSdk");
    std::error_code ec;
    if (!configured.empty() && fs::is_directory(configured, ec)) return configured;
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const char* value = std::getenv(var);
        if (value && *value && fs::is_directory(value, ec)) return value;
    }
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && fs::is_directory(fs::path(local) / "Android" / "Sdk", ec))
        return fs::path(local) / "Android" / "Sdk";
#elif defined(__APPLE__)
    if (home && fs::is_directory(fs::path(home) / "Library" / "Android" / "sdk", ec)) return fs::path(home) / "Library" / "Android" / "sdk";
#else
    if (home && fs::is_directory(fs::path(home) / "Android" / "Sdk", ec)) return fs::path(home) / "Android" / "Sdk";
#endif
    return {};
}

fs::path AndroidTools::jdkHome() {
    const std::string configured = AppSettings::getExternalToolPath("jdk");
    std::error_code ec;
    if (!configured.empty() && fs::is_directory(configured, ec)) return configured;
    const char* javaHome = std::getenv("JAVA_HOME");
    if (javaHome && *javaHome && fs::is_directory(javaHome, ec)) return javaHome;
    // java on PATH: its directory's parent (resolving the alternatives links)
    const fs::path java = ExternalTools::findOnPath("java");
    if (!java.empty()) return fs::weakly_canonical(java, ec).parent_path().parent_path();
    return {};
}

PinnedDownload AndroidTools::bundletoolDownload() {
    PinnedDownload download;
    download.what = std::string("bundletool ") + BUNDLETOOL_VERSION;
    download.url = std::string("https://github.com/google/bundletool/releases/download/") + BUNDLETOOL_VERSION + "/bundletool-all-" +
                   BUNDLETOOL_VERSION + ".jar";
    download.sha256 = BUNDLETOOL_SHA256;
    download.target = App::getUserCacheBaseDir() / "jasperengine" / "tools" / (std::string("bundletool-all-") + BUNDLETOOL_VERSION + ".jar");
    return download;
}

fs::path AndroidTools::bundletoolJar(const std::function<bool()>& cancelled, std::string& error) {
    const PinnedDownload download = bundletoolDownload();
    return fetchPinned(download, cancelled, error) ? download.target : fs::path();
}

AndroidToolchain AndroidTools::discover(unsigned int targetSdk) {
    AndroidToolchain tools;
    tools.sdk = sdkRoot();
    std::error_code ec;
    if (tools.sdk.empty()) {
        tools.missing.push_back("the Android SDK (set Preferences > External Tools > Android SDK, or ANDROID_HOME)");
    } else {
        std::string best;
        for (const auto& entry : fs::directory_iterator(tools.sdk / "build-tools", ec)) {
            const std::string version = entry.path().filename().string();
            if (versionLess(version, MIN_BUILD_TOOLS)) continue;
            if (best.empty() || versionLess(best, version)) best = version;
        }
        if (best.empty()) {
            tools.missing.push_back(std::string("build-tools ") + MIN_BUILD_TOOLS + "+ (SDK Manager package \"build-tools;" + MIN_BUILD_TOOLS + "\")");
        } else {
            tools.buildToolsVersion = best;
            tools.buildTools = tools.sdk / "build-tools" / best;
            const std::string hint = std::string("SDK Manager package \"build-tools;") + MIN_BUILD_TOOLS + "\"";
            tools.aapt2 = inDir(tools.buildTools, "aapt2", "Android SDK", hint);
            tools.zipalign = inDir(tools.buildTools, "zipalign", "Android SDK", hint);
            tools.apksigner = inDir(tools.buildTools, "apksigner", "Android SDK", hint);
            for (const ToolLocation* tool : {&tools.aapt2, &tools.zipalign, &tools.apksigner})
                if (!tool->found()) tools.missing.push_back(tool->name + " (" + hint + ")");
        }
        tools.androidJar = tools.sdk / "platforms" / ("android-" + std::to_string(targetSdk)) / "android.jar";
        if (!fs::is_regular_file(tools.androidJar, ec)) {
            tools.missing.push_back("the platform android-" + std::to_string(targetSdk) + " (SDK Manager package \"platforms;android-" +
                                    std::to_string(targetSdk) + "\")");
            tools.androidJar.clear();
        }
    }
    const fs::path jdk = jdkHome();
    const std::string jdkHint = "a JDK 17 or newer (set Preferences > External Tools > JDK, or JAVA_HOME)";
    tools.java = inDir(jdk / "bin", "java", "JDK", jdkHint);
    tools.keytool = inDir(jdk / "bin", "keytool", "JDK", jdkHint);
    tools.jarsigner = inDir(jdk / "bin", "jarsigner", "JDK", jdkHint);
    for (const ToolLocation* tool : {&tools.java, &tools.keytool, &tools.jarsigner})
        if (!tool->found()) tools.missing.push_back(tool->name + " (" + jdkHint + ")");
    return tools;
}

bool AndroidTools::verifyApk(const fs::path& apk, std::string& output, std::string& error) {
    const AndroidToolchain tools = discover(0);
    if (!tools.apksigner.found()) {
        error = "apksigner not found: " + tools.apksigner.hint;
        return false;
    }
    CommandRunner runner;
    int code = -1;
    output.clear();
    runner.runArgs({tools.apksigner.path.string(), "verify", "--verbose", apk.string()}, apk.parent_path(),
                   [&output](const std::string& line) { output += line + "\n"; }, &code);
    return code == 0;
}
