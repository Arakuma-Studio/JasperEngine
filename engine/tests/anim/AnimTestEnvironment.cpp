// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimTestEnvironment.h"

#include <gtest/gtest.h>

#include "Engine.h"
#include "System.h"

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

    class AnimTestSystem : public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
        std::string getAssetPath() override { return animtest::assetDir(); }
    };

    class EngineEnvironment : public ::testing::Environment {
    public:
        void SetUp() override {
            static bool started = false;   // the engine starts once per process
            if (started){
                return;
            }
            started = true;
            static char name[] = "jasperengine-anim-tests";
            static char* argv[] = {name, nullptr};
            Engine::systemInit(1, argv, new AnimTestSystem());   // owned by the engine for the process
        }

        void TearDown() override {
            std::error_code ignored;
            std::filesystem::remove_all(animtest::assetDir(), ignored);
        }
    };

    // gtest owns the environment; registered when this file is loaded
    const ::testing::Environment* const engineEnvironment =
        ::testing::AddGlobalTestEnvironment(new EngineEnvironment());

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
