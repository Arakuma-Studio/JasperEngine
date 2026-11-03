// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Android side of the native plugins export (feature native-bridge): the jasperbridge
// module's SDK levels, the generated `nativeplugins` Gradle module (the sources of
// plugins/native/android, their registry and keep rules, the dependency settings), the root
// build.gradle repositories, gradle.properties, settings.gradle, the app module's
// dependencies, and the manifest additions. Called by Exporter::writeAndroidProjectSettings on
// the freshly copied workspace (engine/workspaces/androidstudio), so every export writes the
// same files from the same inputs.

#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::NativeExportAndroid {

    namespace fs = std::filesystem;

    // A top-level class / object / interface of a plugin source
    struct PluginClass {
        std::string simpleName;
        std::string qualifiedName;   // package + "." + simpleName (just the name without a package)
        std::string packageName;
        std::string file;            // relative to plugins/native/android, generic separators
    };

    // The public top-level types of a Java source, or the top-level classes and objects of a
    // Kotlin one (internal/private ones are left out: reflection cannot call them)
    std::vector<PluginClass> parseSource(const std::string& source, bool kotlin, const std::string& file);

    // Every plugin class below `androidDir` (.java and .kt files); false with `error` when two
    // files declare the same simple name
    bool collectClasses(const fs::path& androidDir, std::vector<PluginClass>& out, std::string& error);

    // com.jaspergameengine.nativeplugins.JasperNativeRegistry: CLASSES, simple name -> class name
    std::string registrySource(const std::vector<PluginClass>& classes);
    // the consumer keep rules: every plugin package, the bridge, then the folder's own rules
    std::string keepRules(const std::vector<PluginClass>& classes, const std::string& userRules);

    // A dependency setting as a Gradle line: "group:name:version" -> `<configuration> "..."`,
    // any other text verbatim
    std::string dependencyLine(const std::string& entry, const std::string& configuration);
    // gradle.properties with `lines` ("key=value") set: an existing key is replaced in place
    std::string withGradleProperties(const std::string& text, const std::vector<std::string>& lines);

    struct Result {
        // inside <manifest>, before <application> / inside <application>
        std::string permissionsXml;
        std::string manifestXml;
        std::string applicationXml;
        bool needsToolsNamespace = false;
    };

    // workspaceDir: <target>/workspaces/androidstudio; exportProjectRoot: <target>/project (with
    // plugins/native/android copied in). Edits appGradle (the app module's build.gradle text, written
    // by the caller); writes the other files. False with `error` on a problem the export must stop for.
    bool write(const fs::path& workspaceDir, const fs::path& exportProjectRoot, const AndroidProjectSettings& settings,
               std::string& appGradle, Result& result, std::string& error);

}
