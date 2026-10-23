// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestEngine.h"

#include "Engine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    CoreTestSystem* engineSystem = nullptr;

    // before the first test: one engine for the process, like a game's
    class EngineEnvironment: public ::testing::Environment {
    public:
        void SetUp() override { testEngine(); }
    };

    ::testing::Environment* const environment = ::testing::AddGlobalTestEnvironment(new EngineEnvironment());

}

CoreTestSystem& jasperengine::test::testEngine(){
    if (!engineSystem){
        // owned by the engine for the rest of the process, like a platform's System
        engineSystem = new CoreTestSystem();
        char name[] = "jasperengine-core-tests";
        char* argv[] = {name, nullptr};
        Engine::systemInit(1, argv, engineSystem);
    }
    return *engineSystem;
}

std::string CoreTestSystem::getUserDataPath(){
    if (!userDataDir.empty()){
        return userDataDir.string();
    }
    const std::filesystem::path dir = std::filesystem::path(JASPER_TEST_BINARY_DIR) / "userdata";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir.string();
}

std::string CoreTestSystem::getCachePath(){
    return getUserDataPath();
}

bool CoreTestSystem::syncFileSystem(){
    syncs++;
    return true;
}

void CoreTestSystem::platformLog(const int type, const char* fmt, va_list args){
    va_list copy;
    va_copy(copy, args);
    char buffer[2048];
    std::vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logs.push_back({type, buffer});
    }
    System::platformLog(type, fmt, args);
}

size_t CoreTestSystem::countLogs(int type, const std::string& text) const{
    std::lock_guard<std::mutex> lock(logMutex);
    size_t count = 0;
    for (const LogEntry& entry : logs){
        if (entry.type == type && (text.empty() || entry.message.find(text) != std::string::npos)){
            count++;
        }
    }
    return count;
}

void CoreTestSystem::clearLogs(){
    std::lock_guard<std::mutex> lock(logMutex);
    logs.clear();
}
