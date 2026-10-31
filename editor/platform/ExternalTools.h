// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The platform tools a build runs (docs/specs/S30-shipping.md D8): found in Preferences >
// External Tools first, then the environment, then PATH. The editor invokes them (argument
// vectors through CommandRunner::runArgs); it reimplements none of them.

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct ToolLocation {
        std::string name;              // "aapt2", "apksigner", "steamcmd"...
        std::filesystem::path path;    // empty when not found
        std::string source;            // "preferences", "ANDROID_HOME", "PATH"...
        std::string hint;              // what installs it (the SDK Manager package)
        bool found() const { return !path.empty(); }
    };

    class ExternalTools {
    public:
        // an executable on PATH (with the platform's extensions)
        static std::filesystem::path findOnPath(const std::string& name);
        // the first that exists: the preference (a file or its bin/ directory), each environment
        // variable's value (with `subdir`), PATH
        static ToolLocation find(const std::string& name, const std::string& preference, const std::vector<std::string>& envVars,
                                 const std::string& subdir, const std::string& hint);

        static ToolLocation steamcmd();
        static ToolLocation rcodesign();
        static ToolLocation codesign();
        static ToolLocation xcodebuild();
        static ToolLocation objcopy();
        static ToolLocation strip();
        static ToolLocation ssh();
        static ToolLocation scp();
    };

}
