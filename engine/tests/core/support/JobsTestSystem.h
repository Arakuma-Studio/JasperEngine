// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_JOBSTESTSYSTEM_H
#define TESTS_JOBSTESTSYSTEM_H

#include "System.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace testsupport {

    // The engine's System for a test or bench binary without a window: logs go to stderr,
    // the asset root is the working directory.
    class JobsTestSystem : public jasperengine::System {
    public:
        int getScreenWidth() override { return 0; }
        int getScreenHeight() override { return 0; }
        std::string getAssetPath() override { return std::filesystem::current_path().string(); }
        std::string getUserDataPath() override { return std::filesystem::current_path().string(); }
        void platformLog(const int type, const char* fmt, va_list args) override{
            std::lock_guard<std::mutex> lock(mutex);
            std::fprintf(stderr, "[log %d] ", type);
            std::vfprintf(stderr, fmt, args);
            std::fprintf(stderr, "\n");
        }

    private:
        std::mutex mutex;
    };

}

#endif // TESTS_JOBSTESTSYSTEM_H
