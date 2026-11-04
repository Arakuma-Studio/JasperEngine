// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project's native plugins folder, plugins/native/ (feature native-bridge,
// docs/mcp/guide/native-bridge.md):
//   android/  java/**.java, kotlin/**.kt (any .java/.kt below android/), libs/*.aar *.jar, res/**,
//             AndroidManifest.xml (a library manifest, merged), proguard-rules.pro
//   apple/    .m .mm .h .c .cpp .swift (iOS and macOS)
//   ios/      the same kinds plus .framework .xcframework .a (iOS only)
//   macos/    the same kinds (macOS only)
//   web/      .js (a <script> each, before the engine's, in name order)
//   cpp/      .cpp .h: a C++ source root of the Play library and every export
// Used by the exports, the Play library, Project Settings > Native Plugins and the
// native_plugins MCP tool.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::NativePlugins {

    namespace fs = std::filesystem;

    // "plugins/native"
    fs::path relativeRoot();
    // <project>/plugins/native
    fs::path root(const fs::path& projectPath);

    // The platform folders, in this order
    const std::vector<std::string>& platformNames();   // android, apple, ios, macos, web, cpp

    struct Scan {
        fs::path root;          // absolute
        bool exists = false;    // plugins/native is a folder
        // files below each platform folder, relative to it, generic separators, sorted;
        // hidden files and folders (".x") and build/ folders are skipped. Folders that are
        // bundles (.framework, .xcframework) are listed as one entry, not their content.
        std::map<std::string, std::vector<std::string>> files;   // platform -> files

        bool has(const std::string& platform) const;
        // the files of a platform with one of the extensions (".java", ...; lowercase)
        std::vector<std::string> filesWith(const std::string& platform, const std::vector<std::string>& extensions) const;
        size_t count(const std::string& platform) const;
    };
    Scan scan(const fs::path& projectPath);

    // A plugin as the sources show it (best effort: for listings, not for the build)
    struct PluginInfo {
        std::string name;
        std::map<std::string, std::vector<std::string>> files;   // platform -> files (relative to plugins/native)
        std::vector<std::string> methods;                        // sorted, unique
    };
    // Every plugin of the folder: Android classes (file names of .java/.kt), Apple classes
    // (file names of .m/.mm/.swift), Web objects (JasperNative.register("Name", ...)), C++
    // plugins (JASPERENGINE_NATIVE_PLUGIN(Name)); methods parsed from the sources
    std::vector<PluginInfo> listPlugins(const Scan& scan);

    // The Lua mocks: <luaDir>/native_mocks/*.lua names (without .lua), sorted
    std::vector<std::string> listMocks(const fs::path& luaDir);

    // Plugin name -> "Java/Kotlin plugins run only on Android" style message for the plugins
    // that `running` cannot run ("android", "ios", "macos" (Xcode), "macos-cmake" (no Swift), "web",
    // "desktop" = Linux/Windows):
    // NativeBridge::setPlatformOnlyPlugins of editor Play and the exports (a call to one fails
    // with "unavailable" there). Plugins with a C++ implementation are not listed.
    std::map<std::string, std::string> platformOnlyPlugins(const std::vector<PluginInfo>& plugins, const std::string& running);

    // The platform the editor's Play runs plugins of: "macos-cmake" on a Mac (its Objective-C
    // plugins are in the Play library), else "desktop"
    std::string editorPlatform();

    // macOS editor Play: Objective-C classes stay loaded for the editor's life (dyld never
    // unloads an image with Objective-C), so plugin sources of apple/ or macos/ changed since the
    // first Play of this editor run warn once that a restart runs the new code. No-op elsewhere.
    void warnStaleObjectiveC(const fs::path& projectPath);

    // The path below plugins/native, as "android", "ios", ... ("" when `path` is not in it)
    std::string platformOf(const fs::path& projectPath, const fs::path& path);

    // CMake lines compiling the Objective-C (and C/C++) sources of plugins/native/apple and
    // macos into `target` on macOS, with -fobjc-arc and the macos.frameworks linked ("" when
    // there are none). rootExpression: where plugins/native is ("${PROJECT_ROOT}/plugins/native");
    // includeDir: the folder of JasperNative.h. Swift is left out (Xcode builds only).
    std::string macosCMake(const Scan& scan, const std::string& target, const std::string& rootExpression,
                           const std::vector<std::string>& frameworks, const std::string& includeDir);

    // "" when `xml` is well-formed element content (several elements, comments and text are
    // fine; the android:, tools: and other prefixes need no declaration), else what is wrong
    // ("line 2: mismatched element ...")
    std::string xmlFragmentError(const std::string& xml);

    // ---- Apple SDK bundles of the plugin folders
    // The binary is a static archive ("!<arch>\n"): linked, not embedded
    bool isStaticArchive(const fs::path& binary);
    // The binary of a .framework (X.framework/X, or Versions/Current/X on macOS); empty if none
    fs::path frameworkBinary(const fs::path& framework);
    // The binary of an .xcframework's first library (its Info.plist AvailableLibraries[0])
    fs::path xcframeworkBinary(const fs::path& xcframework);

    // ---- scaffolding (Create Plugin dialog, native_plugins create)

    struct MethodSpec {
        std::string name;
        bool async = false;
    };

    struct CreateOptions {
        std::string name;                      // a valid identifier (PascalCase recommended)
        std::vector<std::string> platforms;    // android, ios, macos, web, cpp
        std::string androidLanguage = "java";  // java | kotlin
        std::string appleLanguage = "objc";    // objc | swift
        std::string package = "com.example.plugins";   // Java/Kotlin package
        std::vector<MethodSpec> methods;
        bool overwrite = false;
    };

    struct CreatedFile {
        std::string path;       // project-relative, generic separators
        bool overwritten = false;
    };

    // Validates the options ("" when they are fine)
    std::string validate(const CreateOptions& options);
    // The files create() writes, project-relative (generic separators), in write order
    std::vector<std::string> plannedFiles(const fs::path& projectPath, const fs::path& luaPath, const CreateOptions& options);
    // Writes every file of the plugin plus <luaPath>/native_mocks/<Name>.lua (luaPath: the
    // project's absolute Lua folder). Refuses (false, nothing written) when a file exists and
    // overwrite is false.
    bool create(const fs::path& projectPath, const fs::path& luaPath, const CreateOptions& options,
                std::vector<CreatedFile>& written, std::string& error);

}
