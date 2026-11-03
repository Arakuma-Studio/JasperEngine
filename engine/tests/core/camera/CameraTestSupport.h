// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's camera shake and follow suite on the core test binary (its engine is TestMain's,
// its System the support library's TestSystem): the asset root a test writes .shake files to,
// and the warnings logged since the last call.
#pragma once

#include "LogCapture.h"
#include "TestSystem.h"

#include "System.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace cameratest {

    // a fresh temporary asset root (absolute, generic separators, no trailing '/'); the
    // TestSystem resolves asset paths under it while an AssetRoot is alive
    inline const std::string& assetDir(){
        static const std::string dir = []() {
            std::random_device device;
            const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                               ("jasperengine-camera-tests-" + std::to_string(device()));
            std::filesystem::create_directories(path);
            return path.lexically_normal().generic_string();
        }();
        return dir;
    }

    // asset paths resolve under assetDir() for its lifetime (the TestSystem's own root after)
    class AssetRoot {
    public:
        AssetRoot(){ jasperengine::test::TestSystem::get().setAssetPath(assetDir()); }
        ~AssetRoot(){ jasperengine::test::TestSystem::get().setAssetPath(""); }
        AssetRoot(const AssetRoot&) = delete;
        AssetRoot& operator=(const AssetRoot&) = delete;
    };

    // writes `text` to assetDir()/relativePath (directories made); the absolute path
    inline std::string writeAsset(const std::string& relativePath, const std::string& text){
        const std::filesystem::path path = std::filesystem::path(assetDir()) / relativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path.generic_string();
    }

    // the warnings logged (Log::warn) since the last call (or the test's start), oldest first
    inline std::vector<std::string> takeWarnings(){
        static size_t seen = 0;
        static const ::testing::TestInfo* test = nullptr;
        const ::testing::TestInfo* current = ::testing::UnitTest::GetInstance()->current_test_info();
        jasperengine::test::TestSystem& system = jasperengine::test::TestSystem::get();
        if (current != test) {
            test = current;
            // from the test's start (TestMain marks it)
            seen = system.getLogSequence() - jasperengine::test::LogCapture::sinceTestStart().entries().size();
        }
        std::vector<std::string> taken;
        for (const jasperengine::test::TestSystem::LogEntry& entry : system.getLogsSince(seen)) {
            if (entry.type == D_LOG_WARN) taken.push_back(entry.message);
        }
        seen = system.getLogSequence();
        return taken;
    }

}
