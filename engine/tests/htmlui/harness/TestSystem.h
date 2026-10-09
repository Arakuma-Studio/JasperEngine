// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_SYSTEM_H
#define HTMLUI_TEST_SYSTEM_H

#include "System.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // The engine's System for the test binary: a settable screen size, the fixtures as the
    // asset root, user data and caches in the build directory, and a copy of every log line
    // so tests can assert on errors and warnings.
    class TestSystem : public System {
    public:
        struct LogEntry {
            int type;               // D_LOG_VERBOSE, D_LOG_DEBUG, D_LOG_WARN, D_LOG_ERROR, 0 = print
            std::string message;    // formatted, without the priority prefix
        };

        TestSystem();

        // the engine's system instance (Engine::systemInit is called once, in main)
        static TestSystem& get();

        void setScreenSize(int width, int height);

        int getScreenWidth() override;
        int getScreenHeight() override;

#if defined(SOKOL_VULKAN)
        // the device of the headless VulkanContext; the swapchain is never acquired, so
        // nothing is ever presented
        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;
#endif

        std::string getAssetPath() override;      // <source>/fixtures, or the override
        // another asset root (absolute; "" = the fixtures): a test that loads a fixture
        // directory as a project's assets directory (phase-2 golden pages: their import manifest)
        void setAssetPath(const std::string& path);
        std::string getUserDataPath() override;   // <binary>/userdata
        std::string getCachePath() override;      // <binary>/userdata
        std::string getLuaPath() override;        // <source>/fixtures
        std::string getShaderPath() override;     // <binary>/shaders (empty: runtime compilation)
        bool isUserDataLoaded() override;

        void platformLog(const int type, const char* fmt, va_list args) override;

        // recorded for the input tests (phase 1): the base clipboard is kept
        void showVirtualKeyboard(std::wstring text = L"") override;
        void hideVirtualKeyboard() override;
        void setTextInputArea(float x, float y, float width, float height) override;
        void setMouseCursor(CursorType type) override;
        int virtualKeyboardShows = 0;
        int virtualKeyboardHides = 0;
        std::wstring virtualKeyboardText;
        float textInputArea[4] = {0, 0, 0, 0};
        int textInputAreaCalls = 0;
        CursorType mouseCursor = CursorType::ARROW;

        std::vector<LogEntry> getLogs() const;
        // entries of that type whose message contains `text` (all of the type when empty)
        size_t countLogs(int type, const std::string& text = "") const;
        void clearLogs();

    private:
        int screenWidth = 640;
        int screenHeight = 360;
        std::string assetPathOverride;

        mutable std::mutex logMutex;
        std::vector<LogEntry> logs;
    };

} }

#endif //HTMLUI_TEST_SYSTEM_H
