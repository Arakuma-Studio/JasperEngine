// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The engine's System for the camera tests (started once per process): asset-relative paths
// resolve under a fresh temporary directory, so tests can write .shake files; warnings are kept.

#include "CameraTestEnvironment.h"

#include <gtest/gtest.h>

#include "Engine.h"
#include "System.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    std::string makeAssetDir(){
        std::random_device device;
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("jasperengine-camera-tests-" + std::to_string(device()));
        std::filesystem::create_directories(dir);
        return dir.lexically_normal().generic_string();
    }

    std::mutex warningsMutex;
    std::vector<std::string> warnings;

    class CameraTestSystem : public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
        std::string getAssetPath() override { return cameratest::assetDir(); }
        void platformLog(const int type, const char* fmt, va_list args) override {
            if (type == D_LOG_WARN){
                va_list copy;
                va_copy(copy, args);
                char text[1024];
                std::vsnprintf(text, sizeof(text), fmt, copy);
                va_end(copy);
                std::lock_guard<std::mutex> lock(warningsMutex);
                warnings.push_back(text);
            }
            System::platformLog(type, fmt, args);
        }
    };

    class EngineEnvironment : public ::testing::Environment {
    public:
        void SetUp() override {
            static bool started = false;   // the engine starts once per process
            if (started){
                return;
            }
            started = true;
            static char name[] = "jasperengine-camera-tests";
            static char* argv[] = {name, nullptr};
            Engine::systemInit(1, argv, new CameraTestSystem());   // owned by the engine for the process
        }

        void TearDown() override {
            std::error_code ignored;
            std::filesystem::remove_all(cameratest::assetDir(), ignored);
        }
    };

    const ::testing::Environment* const engineEnvironment =
        ::testing::AddGlobalTestEnvironment(new EngineEnvironment());

}

const std::string& cameratest::assetDir(){
    static const std::string dir = makeAssetDir();
    return dir;
}

std::string cameratest::writeAsset(const std::string& relativePath, const std::string& text){
    std::filesystem::path path = std::filesystem::path(assetDir()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path.generic_string();
}

std::vector<std::string> cameratest::takeWarnings(){
    std::lock_guard<std::mutex> lock(warningsMutex);
    std::vector<std::string> taken;
    taken.swap(warnings);
    return taken;
}
