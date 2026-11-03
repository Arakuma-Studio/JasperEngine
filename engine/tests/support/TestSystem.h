// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_SYSTEM_H
#define JASPER_TEST_SYSTEM_H

#include "System.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // The engine's System for the test binaries: a settable screen size, the target's asset
    // directory as the asset root (TestPaths), user data and caches in the build directory,
    // and a copy of every log line so tests can assert on errors and warnings (LogCapture).
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

        // a phone or tablet for the tests that ask (the debug overlay's scale); off by default
        bool touchDevice = false;
        float screenDpi = 0.0f;
        bool isTouchDevice() override { return touchDevice; }
        float getScreenDpi() override { return screenDpi; }

#if defined(SOKOL_VULKAN) && defined(JASPER_TEST_HAS_GPU)
        // the device of the headless VulkanContext; the swapchain is never acquired, so
        // nothing is ever presented
        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;
#elif defined(SOKOL_METAL) && defined(JASPER_TEST_HAS_GPU)
        // the headless Metal device; no swapchain pass is ever begun (frames render into the
        // GpuEnvironment target)
        sg_environment getSokolEnvironment() override;
#endif

        std::string getAssetPath() override;      // TestPaths::assetDir, or the override
        // another asset root (absolute; "" = TestPaths::assetDir): a test that loads a fixture
        // directory as a project's assets directory (phase-2 golden pages: their import manifest)
        void setAssetPath(const std::string& path);
        std::string getUserDataPath() override;   // <binary>/userdata, or the override
        void setUserDataPath(const std::string& path);  // empty: back to the default
        // Live paths ahead of the overrides (editor tests on a headless Project: its assets,
        // Lua and user data folders, read on every call as the editor's Platform reads them);
        // an empty function: the overrides / defaults. clearPathProvider resets all three.
        struct PathProvider {
            std::function<std::string()> assets;
            std::function<std::string()> lua;
            std::function<std::string()> userData;
        };
        void setPathProvider(PathProvider provider);
        void clearPathProvider();
        bool syncFileSystem() override;           // counts the calls
        int fileSystemSyncs = 0;
        std::string getCachePath() override;      // <binary>/userdata
        std::string getCrashReportPath() override;   // "": the unit tests install no crash handler
        std::string getLuaPath() override;        // TestPaths::assetDir
        std::string getShaderPath() override;     // <binary>/shaders (empty: runtime compilation)
        bool isUserDataLoaded() override;

        void platformLog(const int type, const char* fmt, va_list args) override;
        // the engine's log reaches platformLog through a PlatformSink; the readers below
        // flush the logger first, so what was logged before them is there
        void installLogSinks() override;

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

        // lines logged since the process started, clearLogs included: a position for getLogsSince
        size_t getLogSequence() const;
        // the entries logged at or after that position that clearLogs has not removed
        std::vector<LogEntry> getLogsSince(size_t sequence) const;

    private:
        int screenWidth = 640;
        int screenHeight = 360;
        std::string assetPathOverride;
        std::string userDataPathOverride;
        PathProvider pathProvider;

        mutable std::mutex logMutex;
        std::vector<LogEntry> logs;
        size_t clearedLogs = 0;     // entries clearLogs removed: logs[0] is entry number clearedLogs
    };

} }

#endif //JASPER_TEST_SYSTEM_H
