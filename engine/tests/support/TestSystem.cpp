// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestSystem.h"

#include "TestPaths.h"
#include "log/LogSinks.h"

#if defined(SOKOL_VULKAN) && defined(JASPER_TEST_HAS_GPU)
#include "VulkanContext.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;

TestSystem::TestSystem(){
    std::error_code error;
    std::filesystem::create_directories(getUserDataPath(), error);
    std::filesystem::create_directories(getShaderPath(), error);
}

TestSystem& TestSystem::get(){
    return static_cast<TestSystem&>(System::instance());
}

void TestSystem::setScreenSize(int width, int height){
    screenWidth = width;
    screenHeight = height;
}

int TestSystem::getScreenWidth(){
    return screenWidth;
}

int TestSystem::getScreenHeight(){
    return screenHeight;
}

#if defined(SOKOL_VULKAN) && defined(JASPER_TEST_HAS_GPU)
sg_environment TestSystem::getSokolEnvironment(){
    return VulkanContext::environment();
}

sg_swapchain TestSystem::getSokolSwapchain(){
    return VulkanContext::swapchain();
}
#endif

std::string TestSystem::getAssetPath(){
    if (!assetPathOverride.empty())
        return assetPathOverride;
    return testPaths().assetDir;
}

void TestSystem::setAssetPath(const std::string& path){
    assetPathOverride = path;
}

std::string TestSystem::getCrashReportPath(){
    return std::string();
}

std::string TestSystem::getUserDataPath(){
    if (!userDataPathOverride.empty())
        return userDataPathOverride;
    return testPaths().binaryDir + "/userdata";
}

void TestSystem::setUserDataPath(const std::string& path){
    userDataPathOverride = path;
}

bool TestSystem::syncFileSystem(){
    fileSystemSyncs++;
    return true;
}

std::string TestSystem::getCachePath(){
    return getUserDataPath();
}

std::string TestSystem::getLuaPath(){
    return getAssetPath();
}

std::string TestSystem::getShaderPath(){
    return testPaths().binaryDir + "/shaders";
}

bool TestSystem::isUserDataLoaded(){
    return true;
}

void TestSystem::installLogSinks(){
    // every line is recorded, Log::verbose (Trace) included, as before the logger had levels
    Logger::setDefaultLevel(LogLevel::Trace);
    Logger::addSink(std::make_unique<PlatformSink>(*this));
}

void TestSystem::platformLog(const int type, const char* fmt, va_list args){
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

std::vector<TestSystem::LogEntry> TestSystem::getLogs() const{
    Logger::flush();
    std::lock_guard<std::mutex> lock(logMutex);
    return logs;
}

size_t TestSystem::countLogs(int type, const std::string& text) const{
    Logger::flush();
    std::lock_guard<std::mutex> lock(logMutex);
    size_t count = 0;
    for (const LogEntry& entry : logs){
        if (entry.type == type && (text.empty() || entry.message.find(text) != std::string::npos))
            count++;
    }
    return count;
}

void TestSystem::clearLogs(){
    Logger::flush();
    std::lock_guard<std::mutex> lock(logMutex);
    clearedLogs += logs.size();
    logs.clear();
}

size_t TestSystem::getLogSequence() const{
    Logger::flush();
    std::lock_guard<std::mutex> lock(logMutex);
    return clearedLogs + logs.size();
}

std::vector<TestSystem::LogEntry> TestSystem::getLogsSince(size_t sequence) const{
    Logger::flush();
    std::lock_guard<std::mutex> lock(logMutex);
    const size_t first = sequence > clearedLogs ? sequence - clearedLogs : 0;
    if (first >= logs.size())
        return {};
    return std::vector<LogEntry>(logs.begin() + first, logs.end());
}

void TestSystem::showVirtualKeyboard(std::wstring text){
    virtualKeyboardShows++;
    virtualKeyboardText = text;
}

void TestSystem::hideVirtualKeyboard(){
    virtualKeyboardHides++;
}

void TestSystem::setTextInputArea(float x, float y, float width, float height){
    textInputArea[0] = x;
    textInputArea[1] = y;
    textInputArea[2] = width;
    textInputArea[3] = height;
    textInputAreaCalls++;
}

void TestSystem::setMouseCursor(CursorType type){
    mouseCursor = type;
}
