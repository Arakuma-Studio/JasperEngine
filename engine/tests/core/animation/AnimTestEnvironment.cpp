// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimTestEnvironment.h"

#include <gtest/gtest.h>

#include "System.h"
#include "TestSystem.h"

#include <filesystem>
#include <fstream>
#include <random>

using namespace jasperengine;

namespace {

    std::string makeAssetDir(){
        std::random_device device;
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("jasperengine-anim-tests-" + std::to_string(device()));
        std::filesystem::create_directories(dir);
        return dir.lexically_normal().generic_string();
    }

    // where takeWarnings reads from in the test System's log
    size_t warningsFrom = 0;

    bool isAnimationTest(const ::testing::TestInfo& info){
        const std::string file = std::filesystem::path(info.file()).generic_string();
        return file.find("/tests/core/animation/") != std::string::npos;
    }

    // the tests of this folder run on their own asset root; the rest of the binary keeps the
    // test System's (TestPaths::assetDir)
    class AnimationAssetRoot : public ::testing::EmptyTestEventListener {
    public:
        void OnTestStart(const ::testing::TestInfo& info) override {
            if (!isAnimationTest(info)) return;
            test::TestSystem::get().setAssetPath(animtest::assetDir());
            warningsFrom = test::TestSystem::get().getLogSequence();
        }
        void OnTestEnd(const ::testing::TestInfo& info) override {
            if (isAnimationTest(info)) test::TestSystem::get().setAssetPath("");
        }
        void OnTestProgramEnd(const ::testing::UnitTest&) override {
            std::error_code ignored;
            std::filesystem::remove_all(animtest::assetDir(), ignored);
        }
    };

    // gtest owns the listener; appended when this file is loaded
    const bool listenerInstalled = [](){
        ::testing::UnitTest::GetInstance()->listeners().Append(new AnimationAssetRoot());
        return true;
    }();

}

const std::string& animtest::assetDir(){
    static const std::string dir = makeAssetDir();
    return dir;
}

std::string animtest::writeAsset(const std::string& relativePath, const std::string& text){
    std::filesystem::path path = std::filesystem::path(assetDir()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    return path.lexically_normal().generic_string();
}

std::vector<std::string> animtest::takeWarnings(){
    std::vector<std::string> out;
    for (const test::TestSystem::LogEntry& entry : test::TestSystem::get().getLogsSince(warningsFrom)){
        if (entry.type == D_LOG_WARN) out.push_back(entry.message);
    }
    warningsFrom = test::TestSystem::get().getLogSequence();
    return out;
}
