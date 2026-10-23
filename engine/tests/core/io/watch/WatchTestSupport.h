// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers of the file watch tests: a temp folder per test, file writes, waiting.

#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace jasperengine::test {

    // std::filesystem::temp_directory_path()/je-watch-<random>, removed in teardown (S05 §7)
    class WatchTempDir {
    public:
        WatchTempDir() {
            std::random_device device;
            std::mt19937_64 random(device() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
            path = (std::filesystem::temp_directory_path() / ("je-watch-" + std::to_string(random()))).lexically_normal();
            std::filesystem::create_directories(path);
            // the folder as the OS names it (macOS: /private/var/...)
            std::error_code ec;
            const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
            if (!ec) path = canonical;
        }
        ~WatchTempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        WatchTempDir(const WatchTempDir&) = delete;
        WatchTempDir& operator=(const WatchTempDir&) = delete;

        std::filesystem::path path;
    };

    inline void writeText(const std::filesystem::path& path, const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // Polls `done` every 5 ms until it holds or `timeout` passes
    inline bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

}
