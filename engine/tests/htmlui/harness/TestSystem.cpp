// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestSystem.h"

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <filesystem>

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

#if defined(SOKOL_VULKAN)
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
    return JASPER_HTMLUI_TEST_SOURCE_DIR "/fixtures";
}

void TestSystem::setAssetPath(const std::string& path){
    assetPathOverride = path;
}

std::string TestSystem::getUserDataPath(){
    return JASPER_HTMLUI_TEST_BINARY_DIR "/userdata";
}

std::string TestSystem::getCachePath(){
    return getUserDataPath();
}

std::string TestSystem::getLuaPath(){
    return getAssetPath();
}

std::string TestSystem::getShaderPath(){
    return JASPER_HTMLUI_TEST_BINARY_DIR "/shaders";
}

bool TestSystem::isUserDataLoaded(){
    return true;
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
    std::lock_guard<std::mutex> lock(logMutex);
    return logs;
}

size_t TestSystem::countLogs(int type, const std::string& text) const{
    std::lock_guard<std::mutex> lock(logMutex);
    size_t count = 0;
    for (const LogEntry& entry : logs){
        if (entry.type == type && (text.empty() || entry.message.find(text) != std::string::npos))
            count++;
    }
    return count;
}

void TestSystem::clearLogs(){
    std::lock_guard<std::mutex> lock(logMutex);
    logs.clear();
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
