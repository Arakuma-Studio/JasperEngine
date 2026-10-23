// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The engine of jasperengine-core-tests: initialised once, before the first test, without a GPU
// (no view is loaded), with this System. Scenes, their systems and the Lua state are there for
// every suite.

#include "System.h"

#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::test {

    class CoreTestSystem: public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
        // a test may point it at its own directory (empty: the build directory's userdata)
        std::string getUserDataPath() override;
        std::string getCachePath() override;
        bool syncFileSystem() override;
        void platformLog(const int type, const char* fmt, va_list args) override;

        // entries of that type whose message contains `text` (all of the type when empty)
        size_t countLogs(int type, const std::string& text = "") const;
        void clearLogs();

        std::filesystem::path userDataDir;
        int syncs = 0;

    private:
        struct LogEntry {
            int type;
            std::string message;
        };
        mutable std::mutex logMutex;
        std::vector<LogEntry> logs;
    };

    // The engine's System (Engine::systemInit has run)
    CoreTestSystem& testEngine();

}
