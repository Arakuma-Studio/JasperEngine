// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NativeNeeds.h"

#include "Project.h"
#include "players/PlayerRuntimes.h"
#include "util/ApplePrivacy.h"
#include "util/NativePlugins.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

using namespace jasperengine::editor;

namespace {

    std::string needsExtension(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    // "what (a, b, c, +2 more)": the first three names
    std::string needsNamed(const std::string& what, const std::vector<std::string>& names) {
        std::string list;
        for (size_t i = 0; i < names.size() && i < 3; i++) list += (i ? ", " : "") + names[i];
        if (names.size() > 3) list += ", +" + std::to_string(names.size() - 3) + " more";
        return what + " (" + list + ")";
    }

    std::vector<std::string> needsNonEmpty(const std::vector<std::string>& values) {
        std::vector<std::string> out;
        for (const std::string& value : values) {
            if (value.find_first_not_of(" \t") != std::string::npos) out.push_back(value);
        }
        return out;
    }

    bool needsHasText(const std::string& text) {
        return text.find_first_not_of(" \t\r\n") != std::string::npos;
    }

    // project-relative paths of a plugin folder's files
    std::vector<std::string> needsInFolder(const NativePlugins::Scan& scan, const std::string& folder,
                                      const std::function<bool(const std::string&)>& keep) {
        std::vector<std::string> out;
        auto files = scan.files.find(folder);
        if (files == scan.files.end()) return out;
        for (const std::string& file : files->second) {
            if (keep(file)) out.push_back("plugins/native/" + folder + "/" + file);
        }
        return out;
    }

    bool needsIsPrivacyManifest(const std::string& file) {
        const size_t slash = file.find_last_of('/');
        return (slash == std::string::npos ? file : file.substr(slash + 1)) == "PrivacyInfo.xcprivacy";
    }

}

std::string jasperengine::editor::nativeRunningName(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Android: return "android";
        case BuildPlatform::IOS: return "ios";
        case BuildPlatform::Web: return "web";
        case BuildPlatform::MacOS: return "macos-cmake";
        case BuildPlatform::Linux:
        case BuildPlatform::Windows: break;
    }
    return "desktop";
}

NativeNeeds jasperengine::editor::nativeNeedsFor(const Project& project, BuildPlatform platform, const PlayerInfo* player) {
    NativeNeeds needs;
    const NativePlugins::Scan scan = NativePlugins::scan(project.getProjectPath());

    // C++ plugins compile everywhere (the players hold no project code)
    const std::vector<std::string> cpp = needsInFolder(scan, "cpp", [](const std::string& file) {
        const std::string ext = needsExtension(file);
        return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c";
    });
    if (!cpp.empty()) needs.compileReasons.push_back(needsNamed("C++ native plugins", cpp));

    switch (platform) {
        case BuildPlatform::Android: {
            const AndroidProjectSettings& android = project.getAndroidProjectSettings();
            const std::vector<std::string> sources = needsInFolder(scan, "android", [](const std::string& file) {
                const std::string ext = needsExtension(file);
                return ext == ".java" || ext == ".kt";
            });
            const std::vector<std::string> libraries = needsInFolder(scan, "android", [](const std::string& file) {
                const std::string ext = needsExtension(file);
                return ext == ".aar" || ext == ".jar";
            });
            // what else the export's Gradle module reads (NativeExportAndroid): the resources, the
            // manifest and the R8 rules; a README or notes compile nothing
            const std::vector<std::string> others = needsInFolder(scan, "android", [](const std::string& file) {
                return file.rfind("res/", 0) == 0 || file == "AndroidManifest.xml" || file == "proguard-rules.pro";
            });
            if (!sources.empty()) needs.compileReasons.push_back(needsNamed("Java/Kotlin plugins", sources));
            if (!libraries.empty()) needs.compileReasons.push_back(needsNamed("Android plugin libraries", libraries));
            if (!others.empty()) needs.compileReasons.push_back(needsNamed("Android plugin resources, manifest and R8 rules", others));
            const std::vector<std::string> dependencies = needsNonEmpty(android.gradleDependencies);
            const std::vector<std::string> repositories = needsNonEmpty(android.gradleRepositories);
            const std::vector<std::string> properties = needsNonEmpty(android.gradleProperties);
            if (!dependencies.empty()) needs.compileReasons.push_back(needsNamed("Gradle dependencies (android.gradle_dependencies)", dependencies));
            if (!repositories.empty()) needs.compileReasons.push_back(needsNamed("Maven repositories (android.gradle_repositories)", repositories));
            if (!properties.empty()) needs.compileReasons.push_back(needsNamed("Gradle properties (android.gradle_properties)", properties));
            // tools: attributes are merge rules for Gradle's manifest merger, which only Gradle runs
            const std::pair<std::string, const std::string*> fragments[] = {{"android.manifest_xml", &android.manifestXml},
                                                                            {"android.manifest_application_xml", &android.manifestApplicationXml}};
            for (const auto& [setting, xml] : fragments) {
                if (!needsHasText(*xml)) continue;
                if (xml->find("tools:") != std::string::npos) needs.compileReasons.push_back(setting + " uses tools: merge rules (Gradle's manifest merger)");
                else needs.carried.push_back(setting);
            }
            const std::vector<std::string> permissions = needsNonEmpty(android.extraPermissions);
            if (!permissions.empty()) needs.carried.push_back(needsNamed("extra permissions (android.extra_permissions)", permissions));
            break;
        }
        case BuildPlatform::IOS: {
            const IOSProjectSettings& ios = project.getIOSProjectSettings();
            std::vector<std::string> code = needsInFolder(scan, "apple", [](const std::string& file) { return !needsIsPrivacyManifest(file); });
            const std::vector<std::string> iosCode = needsInFolder(scan, "ios", [](const std::string& file) { return !needsIsPrivacyManifest(file); });
            code.insert(code.end(), iosCode.begin(), iosCode.end());
            if (!code.empty()) {
                const bool takesPlugins = !player || std::find(player->features.begin(), player->features.end(), "nativeplugins") != player->features.end();
                if (takesPlugins) needs.carried.push_back(needsNamed("iOS plugins", code));
                else needs.compileReasons.push_back("the installed " + player->id + " player runtime cannot take native plugins (reinstall it)");
            }
            const std::vector<std::string> pods = [&ios] {
                std::vector<std::string> names;
                for (const NativeCocoaPod& pod : ios.cocoapods) {
                    if (!pod.name.empty()) names.push_back(pod.name);
                }
                return names;
            }();
            if (!pods.empty()) needs.compileReasons.push_back(needsNamed("CocoaPods (ios.cocoapods): pod install needs the exported source project", pods));
            std::vector<std::string> packages;
            for (const NativeSwiftPackage& package : ios.swiftPackages) {
                if (!package.url.empty()) packages.push_back(package.url);
            }
            if (!packages.empty()) needs.carried.push_back(needsNamed("Swift packages (ios.swift_packages)", packages));
            const std::vector<std::string> frameworks = needsNonEmpty(ios.frameworks);
            if (!frameworks.empty()) needs.carried.push_back(needsNamed("frameworks (ios.frameworks)", frameworks));
            if (!ios.plistEntries.empty() || needsHasText(ios.plistXml)) needs.carried.push_back("Info.plist entries (ios.plist_entries, ios.plist_xml)");
            if (needsHasText(ios.entitlementsXml)) needs.carried.push_back("entitlements (ios.entitlements_xml)");
            if (ios.deploymentTarget != IOSProjectSettings().deploymentTarget) needs.carried.push_back("deployment target " + ios.deploymentTarget);
            break;
        }
        case BuildPlatform::MacOS: {
            const MacOSProjectSettings& mac = project.getMacOSProjectSettings();
            std::vector<std::string> code = needsInFolder(scan, "apple", [](const std::string& file) { return !needsIsPrivacyManifest(file); });
            const std::vector<std::string> macCode = needsInFolder(scan, "macos", [](const std::string& file) { return !needsIsPrivacyManifest(file); });
            code.insert(code.end(), macCode.begin(), macCode.end());
            if (!code.empty()) needs.compileReasons.push_back(needsNamed("Objective-C/Swift plugins for macOS", code));
            const std::vector<std::string> frameworks = needsNonEmpty(mac.frameworks);
            if (!frameworks.empty()) needs.compileReasons.push_back(needsNamed("frameworks (macos.frameworks)", frameworks));
            if (!mac.plistEntries.empty()) needs.carried.push_back("Info.plist entries (macos.plist_entries)");
            break;
        }
        case BuildPlatform::Web: {
            const std::vector<std::string> scripts = needsInFolder(scan, "web", [](const std::string& file) { return needsExtension(file) == ".js"; });
            if (!scripts.empty()) needs.carried.push_back(needsNamed("Web plugins", scripts));
            const std::vector<std::string> urls = needsNonEmpty(project.getWebProjectSettings().scriptUrls);
            if (!urls.empty()) needs.carried.push_back(needsNamed("SDK scripts (web.script_urls)", urls));
            break;
        }
        case BuildPlatform::Linux:
        case BuildPlatform::Windows:
            break;
    }

    // the privacy manifest's additions (the engine's own entries are in every Apple build)
    if (platform == BuildPlatform::IOS || platform == BuildPlatform::MacOS) {
        const std::string folder = platform == BuildPlatform::IOS ? "ios" : "macos";
        std::vector<std::string> manifests = needsInFolder(scan, "apple", needsIsPrivacyManifest);
        const std::vector<std::string> own = needsInFolder(scan, folder, needsIsPrivacyManifest);
        manifests.insert(manifests.end(), own.begin(), own.end());
        if (!project.getApplePrivacy().empty() || !manifests.empty()) {
            needs.carried.push_back("privacy manifest additions (apple.privacy_*" + std::string(manifests.empty() ? "" : ", plugin manifests") + ")");
        }
    }

    // a Lua mock that answers where the platform has no code of the plugin: fine in Play, a fake
    // SDK in a shipped game
    const std::vector<std::string> mocks = NativePlugins::listMocks(project.getLuaPath());
    const std::set<std::string> mockNames(mocks.begin(), mocks.end());
    const auto only = NativePlugins::platformOnlyPlugins(NativePlugins::listPlugins(scan), nativeRunningName(platform));
    for (const auto& [name, where] : only) {
        if (!mockNames.count(name)) continue;
        needs.mockWarnings.push_back("Native plugin \"" + name + "\" has no code for " + buildPlatformName(platform) + " (" + where +
                                     "): its Lua mock (native_mocks/" + name + ".lua) answers in this build");
    }
    return needs;
}
