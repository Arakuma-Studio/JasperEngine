// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Build And Run on a device (docs/specs/build-and-run-on-device.md D4): one runner per platform
// (an Android device or emulator through adb, an iOS Simulator through simctl, the browser
// through the editor's web server, this computer), shared by Build Settings, build_project,
// run_build and the CLI. A run's log streams into the Console (category "device", "[<device>]
// <line>"); one run per device, a second one for the same device stops the first.

#include "platform/DeviceCommands.h"
#include "platform/DeviceList.h"
#include "platform/DeviceLogPump.h"
#include "platform/DeviceRecords.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct PatchStats {
        size_t files = 0;        // pushed, the manifest among them
        uint64_t bytes = 0;
        size_t deleted = 0;
    };

    struct DeviceRunRequest {
        RunDevice device;
        std::filesystem::path package;      // .apk, simulator .app, web output folder, executable
        std::filesystem::path contentDir;   // the build cache's cooked content (patches, the install record)
        std::vector<std::string> playerArgs;
        bool patch = false;
        std::string appId;                  // Android package, bundle identifier
        std::string executableName;         // the iOS app's process (its log predicate)
        bool development = false;
        bool headless = false;              // no Simulator window, no browser opened
    };

    struct DeviceRunState {
        // Installing, Pushing 3 files (2.1 MB), Running, Exited, Crashed, Stopped
        std::string phase;
        bool running = false;
        bool crashed = false;
        bool stopped = false;
        bool ready = false;                 // the game logged "Engine ready:"
        std::optional<int> exitCode;
        std::vector<std::string> fatalLines;
        std::string url;                    // the web page
        // the log as the run read it (the newest 2000 lines), its errors and counts
        std::vector<std::string> logTail;
        std::vector<std::string> errors;
        size_t logLines = 0;
        size_t errorCount = 0;
        size_t warningCount = 0;
    };

    class DeviceRunner {
    public:
        virtual ~DeviceRunner() = default;
        virtual bool install(const DeviceRunRequest& request, std::string& error) = 0;
        virtual bool patch(const DeviceRunRequest& request, const devicecmd::PatchPlan& plan, int patchNumber, PatchStats& stats,
                           std::string& error) = 0;
        virtual bool launch(const DeviceRunRequest& request, std::string& error) = 0;
        virtual void stop() = 0;
        // stops following the run (its log) and leaves the game running (the CLI returning)
        virtual void release() {}
        virtual bool screenshot(std::vector<uint8_t>& png, std::string& error) = 0;
        // thread-safe; the log itself goes to the Console
        virtual DeviceRunState state() const = 0;
        // the app still answers on the device (pm path, simctl get_app_container)
        virtual bool installed(const std::string& appId) = 0;
    };

    std::unique_ptr<DeviceRunner> makeRunner(const RunDevice& device);

    // R6: whether a run patches, else why it installs in full
    struct PatchDecision {
        bool patch = false;
        std::string why;
    };
    PatchDecision decidePatch(const DeviceRecord* record, const RunDevice& device, bool development, const std::string& playerId,
                              const std::string& playerIdentity, bool runtimeDebuggable);

    // the identity of a player runtime's binaries (its executables map): what a device record
    // compares to tell whether a patch can reuse the installed package
    std::string playerIdentityOf(const std::map<std::string, std::string>& executables);

    struct DeployRequest {
        DeviceRunRequest run;
        std::filesystem::path internalDir;  // the project's .jasperengine (devices.yaml)
        std::string playerId;
        std::string playerIdentity;
        bool debuggable = false;            // the package says android:debuggable
        bool launchOnly = false;            // the device runs this build already: no install, no patch
    };

    struct DeployResult {
        bool ok = false;
        std::string error;
        std::string installed;              // full, patched, current (launched only)
        size_t pushedFiles = 0;
        uint64_t pushedBytes = 0;
        double launchMs = 0.0;
        std::string url;
    };

    // The active runs (never destroyed: their threads may outlive a static destructor)
    class DeviceRuns {
    public:
        static DeviceRuns& get();

        // Stops the device's run, installs or patches, records what it runs (devices.yaml) and
        // launches. Blocks: a build's job or a worker calls it. progress gets each phase.
        DeployResult deploy(const DeployRequest& request, const std::function<void(const std::string&)>& progress = {});

        struct Run {
            RunDevice device;
            DeviceRunState state;
        };
        std::vector<Run> runs() const;
        std::optional<DeviceRunState> state(const std::string& deviceId) const;
        bool stop(const std::string& deviceId);
        bool screenshot(const std::string& deviceId, std::vector<uint8_t>& png, std::string& error);
        // the screenshot saved as <project>/Recordings/Device/<device>-<UTC time>.png
        bool saveScreenshot(const std::string& deviceId, std::filesystem::path& saved, std::string& error);
        // a run that ended (exited, crashed, stopped) leaves the list
        void dismiss(const std::string& deviceId);
        void stopAll();
        // every run released: the games keep running, nothing follows them
        void releaseAll();

        // this computer's player window, captured as run_build does (BuildTools registers it)
        using HostCapture = std::function<bool(long pid, std::vector<uint8_t>& png, std::string& error)>;
        static void setHostCapture(HostCapture capture);
        static const HostCapture& hostCapture();

    private:
        struct Entry {
            RunDevice device;
            std::unique_ptr<DeviceRunner> runner;
            std::filesystem::path projectDir;
        };
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<Entry>> entries;
    };

    // the Console's category of device logs
    inline constexpr const char* DEVICE_LOG_CATEGORY = "device";
    // a batch of a device's lines into the log (category device, "[<name>] " before each)
    void logDeviceLines(const std::vector<DeviceLogLine>& lines);

}
