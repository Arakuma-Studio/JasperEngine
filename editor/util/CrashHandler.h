// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

namespace jasperengine::editor {

    // Compatibility forwarder to the engine-owned diagnostics handler.
    class CrashHandler {
    public:
        static void install(const std::filesystem::path& logFile);
    };

}
