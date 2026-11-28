// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceRunner.h"

#include "EditorHost.h"
#include "HostServices.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "log/Logger.h"
#include "platform/IosProject.h"
#include "platform/WebServe.h"
#include "player/DevelopmentContent.h"
#include "thread/ThreadUtil.h"
#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/Sha256.h"

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::platform {

namespace fs = std::filesystem;

namespace {

    JE_LOG_CATEGORY(logCategoryDevice, DEVICE_LOG_CATEGORY, LogLevel::Trace)

    constexpr size_t DEVICE_TAIL_LINES = 2000;
    constexpr int DEVICE_TOOL_TIMEOUT_MS = 30000;
    constexpr int DEVICE_INSTALL_TIMEOUT_MS = 600000;

    std::string deviceUtcNow(bool compact) {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char text[32];
        std::strftime(text, sizeof(text), compact ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ", &utc);
        return text;
    }

    std::string deviceMegabytes(uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
        return text;
    }

    std::string deviceLastLine(const std::string& output) {
        std::string text = output;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        const size_t newline = text.rfind('\n');
        return newline == std::string::npos ? text : text.substr(newline + 1);
    }

    bool deviceReadFile(const fs::path& path, std::string& bytes) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    // what the device must run: the cooked content's manifest
    bool deviceContentManifest(const fs::path& contentDir, player::ContentManifest& manifest, std::string& error) {
        std::string text;
        if (!deviceReadFile(contentDir / "content-manifest.json", text)) {
            error = "no content-manifest.json in " + FileUtils::pathToGenericUtf8(contentDir) + ": build again";
            return false;
        }
        return player::ContentManifest::parse(text, manifest, error);
    }

    // a line of the game's log that means it crashed or cannot run
    bool deviceFatalLine(const std::string& line) {
        return line.find("FATAL EXCEPTION") != std::string::npos || line.find("Fatal signal") != std::string::npos ||
               line.find("JASPERENGINE CRASH:") != std::string::npos || line.find("(PANIC)") != std::string::npos;
    }

    // ------------------------------------------------------------------ the base of every runner

    class RunnerBase : public DeviceRunner {
    public:
        explicit RunnerBase(const RunDevice& device)
            : device(device), prefix("[" + device.name + "] "), pump([](std::vector<DeviceLogLine>&& batch) {
                  // handed to the main thread once a frame (the editor's; inline in the CLI)
                  auto lines = std::make_shared<std::vector<DeviceLogLine>>(std::move(batch));
                  getEditorHost().enqueueMainThreadTask([lines]() { logDeviceLines(*lines); });
              }) {}

        ~RunnerBase() override { pump.stop(); }

        DeviceRunState state() const override {
            std::lock_guard<std::mutex> lock(stateMutex);
            return runState;
        }

    protected:
        RunDevice device;
        std::string prefix;
        DeviceLogPump pump;
        mutable std::mutex stateMutex;
        DeviceRunState runState;

        void setPhase(const std::string& phase) {
            std::lock_guard<std::mutex> lock(stateMutex);
            runState.phase = phase;
        }

        // a line the run read from the device
        void line(LogLevel level, const std::string& text) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                runState.logLines++;
                runState.logTail.push_back(text);
                if (runState.logTail.size() > DEVICE_TAIL_LINES) runState.logTail.erase(runState.logTail.begin());
                if (level >= LogLevel::Error) {
                    runState.errorCount++;
                    if (runState.errors.size() < 30) runState.errors.push_back(text.size() > 2000 ? text.substr(0, 2000) + "..." : text);
                } else if (level == LogLevel::Warn) {
                    runState.warningCount++;
                }
                if (deviceFatalLine(text)) {
                    runState.crashed = true;
                    if (runState.fatalLines.size() < 30) runState.fatalLines.push_back(text);
                }
                if (!runState.ready && text.find("Engine ready:") != std::string::npos) {
                    runState.ready = true;
                    onReady();
                }
            }
            pump.push(level, prefix + text);
        }

        // the run's end, said once: "[Pixel 7] exited (code 0)", "... crashed: <line>", "... stopped"
        void finish(std::optional<int> code, bool byStop) {
            std::string how;
            LogLevel level = LogLevel::Info;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (!runState.running) return;
                runState.running = false;
                runState.exitCode = code;
                if (byStop) {
                    runState.stopped = true;
                    runState.phase = "Stopped";
                    how = "stopped";
                } else if (runState.crashed || (code && *code != 0 && device.kind == RunDeviceKind::Host)) {
                    runState.crashed = true;
                    runState.phase = "Crashed";
                    how = "crashed: " + (runState.fatalLines.empty() ? (code ? "exit code " + std::to_string(*code) : std::string("ended by a signal"))
                                                                     : runState.fatalLines.front());
                    level = LogLevel::Error;
                } else {
                    runState.phase = "Exited";
                    how = code ? "exited (code " + std::to_string(*code) + ")" : "exited";
                }
            }
            pump.push(level, prefix + how);
            pump.tick(-1.0, true);
        }

        void started() {
            std::lock_guard<std::mutex> lock(stateMutex);
            const std::string url = runState.url;
            runState = DeviceRunState();
            runState.url = url;
            runState.running = true;
            runState.phase = "Running";
        }

        // the game logged "Engine ready:" (state lock held)
        virtual void onReady() {}
    };

    // ------------------------------------------------------------------ Android (adb)

    class AndroidRunner : public RunnerBase {
    public:
        explicit AndroidRunner(const RunDevice& device): RunnerBase(device), adbPath(adbLocation().path.string()) {}
        ~AndroidRunner() override { halt(); }

        bool install(const DeviceRunRequest& request, std::string& error) override {
            if (adbPath.empty()) {
                error = missingAdbRow().why;
                return false;
            }
            halt();
            setPhase("Installing");
            std::string output;
            // no -g: permissions are asked as on a player's phone
            const int code = adb({"install", "-r", "-t", request.package.string()}, output, DEVICE_INSTALL_TIMEOUT_MS);
            if (code != 0 || output.find("Success") == std::string::npos) {
                if (output.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos)
                    error = device.name + " has " + request.appId + " signed with another key: uninstall the app from the device first (adb uninstall " +
                            request.appId + "), or sign with the same keystore";
                else
                    error = "adb install failed on " + device.name + ": " + deviceLastLine(output);
                return false;
            }
            // stale development content never outlives its package (a package without debuggable
            // has none: run-as refuses it, nothing to clear)
            if (request.development) adbRun(devicecmd::runAs(adbPath, device.id, request.appId, devicecmd::clearScript()), output);
            return true;
        }

        bool patch(const DeviceRunRequest& request, const devicecmd::PatchPlan& plan, int patchNumber, PatchStats& stats, std::string& error) override {
            halt();
            const std::string remote = devicecmd::remotePatchDir(request.appId, patchNumber);
            // the staged patch: files/... and the manifest, in the build cache
            const fs::path stagingRoot = request.contentDir.parent_path() / "device-patch";
            const fs::path staging = stagingRoot / ("patch-" + std::to_string(patchNumber));
            std::error_code ec;
            fs::remove_all(stagingRoot, ec);
            fs::create_directories(staging / "files", ec);
            for (const std::string& file : plan.push) {
                const fs::path target = staging / "files" / FileUtils::pathFromUtf8(file);
                fs::create_directories(target.parent_path(), ec);
                fs::copy_file(request.contentDir / FileUtils::pathFromUtf8(file), target, ec);
                if (ec) {
                    error = "cannot stage " + file + " for the patch: " + ec.message();
                    return false;
                }
            }
            std::string manifestText;
            if (!patchedManifest(request.contentDir, patchNumber, manifestText, error)) return false;
            if (!io::writeFileAtomic(staging / "content-manifest.json", manifestText).ok()) {
                error = "cannot stage content-manifest.json for the patch";
                return false;
            }
            stats.files = plan.push.size() + 1;
            stats.bytes = plan.bytes + manifestText.size();
            stats.deleted = plan.remove.size();
            setPhase("Pushing " + std::to_string(stats.files) + " files (" + deviceMegabytes(stats.bytes) + ")");
            std::string output;
            if (adb({"shell", "mkdir -p " + devicecmd::shellQuote(devicecmd::remoteRoot(request.appId))}, output) != 0 ||
                adb({"push", staging.string(), devicecmd::remoteRoot(request.appId) + "/"}, output, DEVICE_INSTALL_TIMEOUT_MS) != 0) {
                error = "adb push to " + device.name + " failed: " + deviceLastLine(output);
                fs::remove_all(stagingRoot, ec);
                return false;
            }
            const int copied = adbRun(devicecmd::runAs(adbPath, device.id, request.appId, devicecmd::patchScript(remote, plan.remove, plan.first)), output);
            adb({"shell", "rm -rf " + devicecmd::shellQuote(remote)}, output);
            fs::remove_all(stagingRoot, ec);
            if (copied != 0) {
                error = "this device's run-as is not usable for " + request.appId + " (" + deviceLastLine(output) +
                        "): Build And Run without patch installs in full";
                return false;
            }
            return true;
        }

        bool launch(const DeviceRunRequest& request, std::string& error) override {
            halt();
            appId = request.appId;
            std::string output;
            adb({"shell", "am force-stop " + devicecmd::shellQuote(appId)}, output);
            // the options through the system property NativeEngine reads, cleared once the game is
            // ready (or after 30 s) so a later start from the launcher runs without them
            std::string words;
            for (const std::string& arg : request.playerArgs) words += (words.empty() ? "" : " ") + arg;
            adb({"shell", "setprop debug.jasperengine.args " + devicecmd::shellQuote(words)}, output);
            argsSet = !words.empty();
            const int code = adb({"shell", "am start -W -n " + devicecmd::shellQuote(appId + "/com.jaspergameengine.jasperengine.MainActivity")}, output);
            if (code != 0 || output.find("Error") != std::string::npos) {
                error = "am start failed on " + device.name + ": " + deviceLastLine(output);
                clearArgs();
                return false;
            }
            std::string pid;
            for (int attempt = 0; attempt < 20 && pid.empty(); attempt++) {
                if (adb({"shell", "pidof " + devicecmd::shellQuote(appId)}, output, 5000) == 0) pid = deviceLastLine(output);
                pid.erase(std::remove_if(pid.begin(), pid.end(), [](char c) { return c < '0' || c > '9'; }), pid.end());
                if (pid.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (pid.empty()) {
                error = appId + " did not start on " + device.name + " (no process): read its logcat";
                clearArgs();
                return false;
            }
            started();
            launchedAt = std::chrono::steady_clock::now();
            running = true;
            pump.start();
            // its log: logcat of its pid (API 24+); an older Android filtered here
            const bool filterHere = device.apiLevel > 0 && device.apiLevel < 24;
            std::vector<std::string> logcat = devicecmd::adb(adbPath, device.id, {"logcat", "-v", "threadtime"});
            if (!filterHere) logcat.push_back("--pid=" + pid);
            const std::string ownPid = pid;
            logThread = std::thread([this, logcat, filterHere, ownPid]() {
                logRunner.run(logcat, fs::path(), [this, filterHere, ownPid](const std::string& text) { logcatLine(text, filterHere ? ownPid : ""); });
            });
            monitorThread = std::thread([this]() { monitor(); });
            return true;
        }

        void stop() override {
            if (!running) return;
            std::string output;
            adb({"shell", "am force-stop " + devicecmd::shellQuote(appId)}, output);
            stopping = true;
            finish(std::nullopt, true);
            halt();
        }

        void release() override {
            stopping = true;
            halt();
        }

        bool screenshot(std::vector<uint8_t>& png, std::string& error) override {
            // a file on the device, then pulled: the editor reads tool output as lines, not bytes
            const std::string remote = "/data/local/tmp/jasper-screenshot.png";
            const fs::path local = fs::temp_directory_path() / ("jasper-device-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
            std::string output;
            if (adb({"shell", "screencap -p " + remote}, output) != 0 || adb({"pull", remote, local.string()}, output) != 0) {
                error = "screencap on " + device.name + " failed: " + deviceLastLine(output);
                return false;
            }
            adb({"shell", "rm -f " + remote}, output);
            std::string bytes;
            const bool read = deviceReadFile(local, bytes);
            std::error_code ec;
            fs::remove(local, ec);
            if (!read || bytes.size() < 8) {
                error = "the screenshot of " + device.name + " is empty";
                return false;
            }
            png.assign(bytes.begin(), bytes.end());
            return true;
        }

        bool installed(const std::string& id) override {
            std::string output;
            return adb({"shell", "pm path " + devicecmd::shellQuote(id)}, output) == 0 && output.find("package:") != std::string::npos;
        }

    private:
        std::string adbPath;
        std::string appId;
        CommandRunner logRunner;
        std::thread logThread;
        std::thread monitorThread;
        std::atomic<bool> running{false};
        std::atomic<bool> stopping{false};
        std::atomic<bool> argsSet{false};
        std::mutex monitorMutex;
        std::condition_variable monitorWake;
        std::chrono::steady_clock::time_point launchedAt;

        int adb(const std::vector<std::string>& args, std::string& output, int timeoutMs = DEVICE_TOOL_TIMEOUT_MS) {
            return CommandRunner::captureResult(devicecmd::adb(adbPath, device.id, args), output, timeoutMs);
        }
        int adbRun(const std::vector<std::string>& argv, std::string& output) {
            return CommandRunner::captureResult(argv, output, DEVICE_INSTALL_TIMEOUT_MS);
        }

        void clearArgs() {
            if (!argsSet.exchange(false)) return;
            std::string output;
            adb({"shell", "setprop debug.jasperengine.args ''"}, output);
        }

        void onReady() override {
            // (state lock held) the property goes on the monitor thread
            monitorWake.notify_all();
        }

        // "11-20 10:00:00.123  1234  1250 I JasperEngine: text"
        void logcatLine(const std::string& text, const std::string& onlyPid) {
            static const std::regex threadtime(R"(^\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+(\d+)\s+\d+\s+([VDIWEFA])\s+(.*?)\s*: (.*)$)");
            std::smatch match;
            if (!std::regex_match(text, match, threadtime)) {
                if (text.rfind("---------", 0) == 0) return;
                line(LogLevel::Info, text);
                return;
            }
            if (!onlyPid.empty() && match[1].str() != onlyPid) return;
            LogLevel level = LogLevel::Info;
            switch (match[2].str()[0]) {
                case 'V': level = LogLevel::Trace; break;
                case 'D': level = LogLevel::Debug; break;
                case 'W': level = LogLevel::Warn; break;
                case 'E': level = LogLevel::Error; break;
                case 'F':
                case 'A': level = LogLevel::Fatal; break;
                default: break;
            }
            const std::string tag = match[3].str();
            line(level, tag == "JasperEngine" ? match[4].str() : tag + ": " + match[4].str());
        }

        // the process gone (pidof empty) ends the run; the launch options go once ready or after 30 s
        void monitor() {
            std::unique_lock<std::mutex> lock(monitorMutex);
            while (running && !stopping) {
                monitorWake.wait_for(lock, std::chrono::seconds(1));
                if (!running || stopping) break;
                lock.unlock();
                if (argsSet && (state().ready || std::chrono::steady_clock::now() - launchedAt > std::chrono::seconds(30))) clearArgs();
                std::string output;
                const int code = adb({"shell", "pidof " + devicecmd::shellQuote(appId)}, output, 5000);
                lock.lock();
                if (stopping) break;
                std::string pid = deviceLastLine(output);
                pid.erase(std::remove_if(pid.begin(), pid.end(), [](char c) { return c < '0' || c > '9'; }), pid.end());
                if (code >= 0 && pid.empty()) {
                    lock.unlock();
                    // what logcat still had
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    clearArgs();
                    finish(std::nullopt, false);
                    lock.lock();
                    running = false;
                    break;
                }
            }
        }

        void halt() {
            running = false;
            monitorWake.notify_all();
            logRunner.cancel();
            if (logThread.joinable() && logThread.get_id() != std::this_thread::get_id()) logThread.join();
            if (monitorThread.joinable() && monitorThread.get_id() != std::this_thread::get_id()) monitorThread.join();
            stopping = false;
            pump.stop();
        }

    public:
        // the cooked manifest with the patch counter and its time (R8): written last on the device
        static bool patchedManifest(const fs::path& contentDir, int patchNumber, std::string& text, std::string& error) {
            player::ContentManifest manifest;
            if (!deviceContentManifest(contentDir, manifest, error)) return false;
            manifest.patch = patchNumber;
            manifest.patchedAt = deviceUtcNow(false);
            text = manifest.toJson().dump(2) + "\n";
            return true;
        }
    };

    // ------------------------------------------------------------------ iOS Simulator (simctl)

    class SimulatorRunner : public RunnerBase {
    public:
        explicit SimulatorRunner(const RunDevice& device): RunnerBase(device) {
            const XcodeChoice xcode = findXcode();
            if (xcode.found() && xcode.throughEnvironment) {
                env = CommandRunner::currentEnvironment();
                CommandRunner::setVariable(env, "DEVELOPER_DIR", xcode.developerDir.string());
                withEnv = true;
            }
        }
        ~SimulatorRunner() override { halt(); }

        bool install(const DeviceRunRequest& request, std::string& error) override {
            halt();
            setPhase("Installing");
            std::string output;
            if (device.state != "Booted") {
                simctl({"boot", device.id}, output);
                if (simctl({"bootstatus", device.id, "-b"}, output, DEVICE_INSTALL_TIMEOUT_MS) != 0) {
                    error = device.name + " did not boot: " + deviceLastLine(output);
                    return false;
                }
            }
            if (!request.headless) CommandRunner::captureResult({"open", "-a", "Simulator"}, output, 10000);
            if (simctl({"install", device.id, request.package.string()}, output, DEVICE_INSTALL_TIMEOUT_MS) != 0) {
                error = "simctl install on " + device.name + " failed: " + deviceLastLine(output);
                return false;
            }
            const fs::path folder = developmentFolder(request.appId);
            std::error_code ec;
            if (!folder.empty()) fs::remove_all(folder, ec);
            return true;
        }

        bool patch(const DeviceRunRequest& request, const devicecmd::PatchPlan& plan, int patchNumber, PatchStats& stats, std::string& error) override {
            halt();
            const fs::path folder = developmentFolder(request.appId);
            if (folder.empty()) {
                error = request.appId + " is not installed on " + device.name + ": Build And Run without patch";
                return false;
            }
            std::string manifestText;
            if (!AndroidRunner::patchedManifest(request.contentDir, patchNumber, manifestText, error)) return false;
            stats.files = plan.push.size() + 1;
            stats.bytes = plan.bytes + manifestText.size();
            stats.deleted = plan.remove.size();
            setPhase("Pushing " + std::to_string(stats.files) + " files (" + deviceMegabytes(stats.bytes) + ")");
            // the data container is a folder of this Mac: written directly, the manifest last
            std::error_code ec;
            if (plan.first) fs::remove_all(folder, ec);
            for (const std::string& file : plan.push) {
                std::string bytes;
                if (!deviceReadFile(request.contentDir / FileUtils::pathFromUtf8(file), bytes) ||
                    !io::writeFileAtomic(folder / FileUtils::pathFromUtf8(file), bytes).ok()) {
                    error = "cannot write " + file + " into " + device.name + "'s app container";
                    return false;
                }
            }
            for (const std::string& file : plan.remove) fs::remove(folder / FileUtils::pathFromUtf8(file), ec);
            if (!io::writeFileAtomic(folder / "content-manifest.json", manifestText).ok()) {
                error = "cannot write content-manifest.json into " + device.name + "'s app container";
                return false;
            }
            return true;
        }

        bool launch(const DeviceRunRequest& request, std::string& error) override {
            halt();
            appId = request.appId;
            std::string output;
            const fs::path logs = request.contentDir.empty() ? fs::temp_directory_path() : request.contentDir.parent_path();
            stdoutFile = logs / "simulator-stdout.log";
            stderrFile = logs / "simulator-stderr.log";
            std::error_code ec;
            fs::remove(stdoutFile, ec);
            fs::remove(stderrFile, ec);
            std::string executable = request.executableName.empty() ? request.package.stem().string() : request.executableName;
            // a patch run has no package: the installed app's bundle names the process
            if (executable.empty() && simctl({"get_app_container", device.id, appId, "app"}, output) == 0)
                executable = FileUtils::pathFromUtf8(deviceLastLine(output)).stem().string();
            std::vector<std::string> args = {"launch", "--terminate-running-process", "--stdout=" + stdoutFile.string(), "--stderr=" + stderrFile.string(),
                                             device.id, appId};
            args.insert(args.end(), request.playerArgs.begin(), request.playerArgs.end());
            if (simctl(args, output) != 0) {
                error = "simctl launch on " + device.name + " failed: " + deviceLastLine(output);
                return false;
            }
            // "com.example.game: 12345"
            const size_t colon = output.rfind(':');
            pid = colon == std::string::npos ? 0 : std::atol(output.substr(colon + 1).c_str());
            merge = std::make_unique<SimulatorLogMerge>(appId, [this](LogLevel level, const std::string& text) { line(level, text); });
            started();
            running = true;
            pump.start();
            std::vector<std::string> stream = {"xcrun", "simctl", "spawn", device.id, "log", "stream", "--style", "compact", "--level", "debug",
                                               "--predicate", "process == \"" + executable + "\""};
            logThread = std::thread([this, stream]() {
                logRunner.run(stream, fs::path(), [this](const std::string& text) { merge->unifiedLine(text); }, withEnv ? &env : nullptr);
            });
            monitorThread = std::thread([this]() { monitor(); });
            return true;
        }

        void stop() override {
            if (!running) return;
            std::string output;
            simctl({"terminate", device.id, appId}, output);
            if (merge) merge->flush(-1.0, true);
            finish(std::nullopt, true);
            halt();
        }

        void release() override { halt(); }

        bool screenshot(std::vector<uint8_t>& png, std::string& error) override {
            const fs::path local = fs::temp_directory_path() / ("jasper-simulator-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
            std::string output;
            if (simctl({"io", device.id, "screenshot", local.string()}, output) != 0) {
                error = "simctl io screenshot on " + device.name + " failed: " + deviceLastLine(output);
                return false;
            }
            std::string bytes;
            const bool read = deviceReadFile(local, bytes);
            std::error_code ec;
            fs::remove(local, ec);
            if (!read || bytes.empty()) {
                error = "the screenshot of " + device.name + " is empty";
                return false;
            }
            png.assign(bytes.begin(), bytes.end());
            return true;
        }

        bool installed(const std::string& id) override {
            std::string output;
            return simctl({"get_app_container", device.id, id, "data"}, output) == 0;
        }

    private:
        CommandRunner::Environment env;
        bool withEnv = false;
        std::string appId;
        long pid = 0;
        fs::path stdoutFile, stderrFile;
        uint64_t stdoutRead = 0, stderrRead = 0;
        std::unique_ptr<SimulatorLogMerge> merge;
        CommandRunner logRunner;
        std::thread logThread, monitorThread;
        std::atomic<bool> running{false};

        int simctl(std::vector<std::string> args, std::string& output, int timeoutMs = DEVICE_TOOL_TIMEOUT_MS) {
            args.insert(args.begin(), {"xcrun", "simctl"});
            return CommandRunner::captureResult(args, output, timeoutMs, withEnv ? &env : nullptr);
        }

        // <data container>/Library/je-dev-content, "" when the app is not installed
        fs::path developmentFolder(const std::string& id) {
            std::string output;
            if (simctl({"get_app_container", device.id, id, "data"}, output) != 0) return {};
            const std::string container = deviceLastLine(output);
            return container.empty() ? fs::path() : FileUtils::pathFromUtf8(container) / "Library" / player::DEVELOPMENT_CONTENT_DIR;
        }

        // the launch's stdout and stderr files, read on as they grow
        void tail(const fs::path& file, uint64_t& offset, bool stderrStream) {
            std::ifstream in(file, std::ios::binary);
            if (!in) return;
            in.seekg(static_cast<std::streamoff>(offset));
            std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t complete = chunk.rfind('\n');
            if (complete == std::string::npos) return;
            offset += complete + 1;
            std::istringstream lines(chunk.substr(0, complete));
            for (std::string text; std::getline(lines, text);) merge->consoleLine(text, stderrStream);
        }

        void monitor() {
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!running) break;
                tail(stdoutFile, stdoutRead, false);
                tail(stderrFile, stderrRead, true);
                merge->flush();
#ifndef _WIN32
                // a simulator app is a process of this Mac
                if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    tail(stdoutFile, stdoutRead, false);
                    tail(stderrFile, stderrRead, true);
                    merge->flush(-1.0, true);
                    finish(std::nullopt, false);
                    running = false;
                    break;
                }
#endif
            }
        }

        void halt() {
            running = false;
            logRunner.cancel();
            if (logThread.joinable() && logThread.get_id() != std::this_thread::get_id()) logThread.join();
            if (monitorThread.joinable() && monitorThread.get_id() != std::this_thread::get_id()) monitorThread.join();
            stdoutRead = stderrRead = 0;
            pump.stop();
        }
    };

    // ------------------------------------------------------------------ the browser (WebServe)

    class WebRunner : public RunnerBase {
    public:
        explicit WebRunner(const RunDevice& device): RunnerBase(device) {}
        ~WebRunner() override {
            WebServe::get().setHandlers(nullptr, nullptr);
            pump.stop();
        }

        bool install(const DeviceRunRequest& request, std::string& error) override {
            const fs::path folder = fs::is_directory(request.package) ? request.package : request.package.parent_path();
            if (!WebServe::get().serve(folder, error)) return false;
            WebServe::get().setHandlers(
                [this](const std::vector<WebServe::PageLine>& lines, const std::string& crash) {
                    for (const WebServe::PageLine& page : lines) {
                        const LogLevel level = page.level == "error" ? LogLevel::Error : page.level == "warn" ? LogLevel::Warn
                                             : page.level == "debug" ? LogLevel::Debug : LogLevel::Info;
                        line(level, page.text);
                    }
                    if (!crash.empty()) line(LogLevel::Fatal, "JASPERENGINE CRASH: " + crash);
                },
                [this](std::vector<uint8_t>&& png) {
                    std::lock_guard<std::mutex> lock(shotMutex);
                    shot = std::move(png);
                    shotReady = true;
                    shotWake.notify_all();
                });
            return true;
        }

        bool patch(const DeviceRunRequest&, const devicecmd::PatchPlan&, int, PatchStats&, std::string& error) override {
            error = "Patch And Run is for Android and the iOS Simulator";
            return false;
        }

        bool launch(const DeviceRunRequest& request, std::string& error) override {
            const int port = WebServe::get().port();
            if (port == 0) {
                error = "the web server is not running";
                return false;
            }
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/index.html?editor-port=" + std::to_string(port);
            const std::string query = devicecmd::queryFromArgs(request.playerArgs);
            if (!query.empty()) url += "&" + query;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                runState.url = url;
            }
            started();
            pump.start();
            if (!request.headless && !HostServices::openURL(url)) line(LogLevel::Warn, "no browser opened: open " + url);
            return true;
        }

        void stop() override {
            WebServe::get().queueCommand("quit");
            finish(std::nullopt, true);
            pump.stop();
        }

        bool screenshot(std::vector<uint8_t>& png, std::string& error) override {
            std::unique_lock<std::mutex> lock(shotMutex);
            shotReady = false;
            WebServe::get().queueCommand("screenshot");
            if (!shotWake.wait_for(lock, std::chrono::seconds(10), [this]() { return shotReady; })) {
                error = "the page sent no screenshot (is it open, and a development build?)";
                return false;
            }
            png = std::move(shot);
            return true;
        }

        bool installed(const std::string&) override { return true; }

    private:
        std::mutex shotMutex;
        std::condition_variable shotWake;
        std::vector<uint8_t> shot;
        bool shotReady = false;
    };

    // ------------------------------------------------------------------ this computer

    class HostRunner : public RunnerBase {
    public:
        explicit HostRunner(const RunDevice& device): RunnerBase(device) {}
        ~HostRunner() override { halt(); }

        bool install(const DeviceRunRequest&, std::string&) override { return true; }

        bool patch(const DeviceRunRequest&, const devicecmd::PatchPlan&, int, PatchStats&, std::string& error) override {
            error = "Patch And Run is for Android and the iOS Simulator";
            return false;
        }

        bool launch(const DeviceRunRequest& request, std::string& error) override {
            halt();
            fs::path executable = request.package;
            // a macOS bundle's own executable, so its output comes through a pipe
            if (executable.extension() == ".app") executable = executable / "Contents" / "MacOS" / executable.stem();
            std::error_code ec;
            if (!fs::is_regular_file(executable, ec)) {
                error = "no player at " + FileUtils::pathToGenericUtf8(executable);
                return false;
            }
            std::vector<std::string> argv;
#ifndef _WIN32
            // a player's stdout into a pipe is block-buffered: line-buffered through stdbuf
            for (const char* stdbuf : {"/usr/bin/stdbuf", "/bin/stdbuf"}) {
                if (::access(stdbuf, X_OK) == 0) {
                    argv = {stdbuf, "-oL", "-eL"};
                    break;
                }
            }
#endif
            argv.push_back(executable.string());
            argv.insert(argv.end(), request.playerArgs.begin(), request.playerArgs.end());
            started();
            pump.start();
            const fs::path workDir = executable.parent_path();
            playerThread = std::thread([this, argv, workDir]() {
                int code = -1;
                playerRunner.runArgs(argv, workDir, [this](const std::string& text) {
                    line(text.find("(ERROR)") != std::string::npos ? LogLevel::Error : text.rfind("(WARN)", 0) == 0 ? LogLevel::Warn : LogLevel::Info, text);
                }, &code);
                const bool byStop = playerRunner.isCancelRequested();
                if (!byStop && code < 0) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    runState.crashed = true;
                }
                finish(code < 0 ? std::nullopt : std::optional<int>(code), byStop);
            });
            return true;
        }

        void stop() override { halt(); }

        bool screenshot(std::vector<uint8_t>& png, std::string& error) override {
            const DeviceRuns::HostCapture& capture = DeviceRuns::hostCapture();
            if (!capture) {
                error = "this computer's player cannot be captured here";
                return false;
            }
            return capture(playerRunner.processId(), png, error);
        }

        bool installed(const std::string&) override { return true; }

    private:
        CommandRunner playerRunner;
        std::thread playerThread;

        void halt() {
            playerRunner.cancel();
            if (playerThread.joinable() && playerThread.get_id() != std::this_thread::get_id()) playerThread.join();
            pump.stop();
        }
    };

    DeviceRuns::HostCapture& hostCaptureSlot() {
        static DeviceRuns::HostCapture* capture = new DeviceRuns::HostCapture();
        return *capture;
    }

}

void logDeviceLines(const std::vector<DeviceLogLine>& lines) {
    LogCategory& category = logCategoryDevice();
    for (const DeviceLogLine& entry : lines) {
        if (!Logger::shouldLog(entry.level, category)) continue;
        // the pump rate-limits device lines itself
        Logger::write(entry.level, category, entry.text, nullptr, LogFlagNoRateLimit | LogFlagNoBacktrace);
    }
}

std::unique_ptr<DeviceRunner> makeRunner(const RunDevice& device) {
    switch (device.kind) {
        case RunDeviceKind::Host: return std::make_unique<HostRunner>(device);
        case RunDeviceKind::Browser: return std::make_unique<WebRunner>(device);
        case RunDeviceKind::Simulator: return std::make_unique<SimulatorRunner>(device);
        case RunDeviceKind::Device:
        case RunDeviceKind::Emulator: return std::make_unique<AndroidRunner>(device);
    }
    return nullptr;
}

std::string playerIdentityOf(const std::map<std::string, std::string>& executables) {
    std::string listing;
    for (const auto& [path, sha] : executables) listing += path + "=" + sha + "\n";
    return sha256Hex(listing);
}

PatchDecision decidePatch(const DeviceRecord* record, const RunDevice& device, bool development, const std::string& playerId,
                                    const std::string& playerIdentity, bool runtimeDebuggable) {
    PatchDecision decision;
    if (device.platform != BuildPlatform::Android && device.platform != BuildPlatform::IOS) {
        decision.why = "Patch And Run is for Android and the iOS Simulator";
    } else if (!development) {
        decision.why = "Patch And Run needs a Development Build";
    } else if (device.platform == BuildPlatform::Android && !runtimeDebuggable) {
        decision.why = "the Android player runtime predates Patch And Run: update it";
    } else if (!record) {
        decision.why = "no install recorded on " + device.name;
    } else if (record->playerId != playerId || record->playerSha256 != playerIdentity) {
        decision.why = "the player binary changed (" + (record->playerId.empty() ? std::string("none") : record->playerId) +
                       (record->playerId == playerId ? ", rebuilt" : " -> " + playerId) + ")";
    } else if (!record->development) {
        decision.why = "the installed package is not a Development Build";
    } else if (device.platform == BuildPlatform::Android && !record->debuggable) {
        decision.why = "the installed package is not debuggable";
    } else {
        decision.patch = true;
    }
    return decision;
}

DeviceRuns& DeviceRuns::get() {
    static DeviceRuns* instance = new DeviceRuns();
    return *instance;
}

void DeviceRuns::setHostCapture(HostCapture capture) {
    hostCaptureSlot() = std::move(capture);
}

const DeviceRuns::HostCapture& DeviceRuns::hostCapture() {
    return hostCaptureSlot();
}

DeployResult DeviceRuns::deploy(const DeployRequest& request, const std::function<void(const std::string&)>& progress) {
    DeployResult result;
    const DeviceRunRequest& run = request.run;
    const RunDevice& device = run.device;
    auto say = [&progress](const std::string& text) {
        if (progress) progress(text);
    };

    // one run per device: the earlier one goes first
    std::shared_ptr<Entry> previous;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(device.id);
        if (it != entries.end()) {
            previous = it->second;
            entries.erase(it);
        }
    }
    if (previous && previous->runner) previous->runner->stop();
    previous.reset();

    auto entry = std::make_shared<Entry>();
    entry->device = device;
    entry->runner = makeRunner(device);
    entry->projectDir = request.internalDir.parent_path();
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries[device.id] = entry;
    }
    DeviceRunner& runner = *entry->runner;
    const bool mobile = device.platform == BuildPlatform::Android || device.platform == BuildPlatform::IOS;

    std::string warning;
    DeviceRecords records = mobile ? DeviceRecords::load(request.internalDir, &warning) : DeviceRecords();
    if (!warning.empty()) Out::warning("%s", warning.c_str());
    player::ContentManifest manifest;
    if (mobile && !request.launchOnly && !deviceContentManifest(run.contentDir, manifest, result.error)) return result;

    if (request.launchOnly) {
        result.installed = "current";
    } else if (mobile && run.patch) {
        auto recorded = records.devices.find(device.id);
        if (recorded == records.devices.end()) {
            result.error = "no install recorded on " + device.name + ": Build And Run without patch";
            return result;
        }
        if (!runner.installed(run.appId)) {
            records.devices.erase(recorded);
            std::string ignored;
            records.save(request.internalDir, ignored);
            result.error = run.appId + " is no longer installed on " + device.name + ": Build And Run without patch for a full install";
            return result;
        }
        const devicecmd::PatchPlan plan = devicecmd::planPatch(recorded->second.content, manifest.files, recorded->second.patch == 0);
        const int number = recorded->second.patch + 1;
        say("Pushing " + std::to_string(plan.push.size() + 1) + " files to " + device.name);
        PatchStats stats;
        if (!runner.patch(run, plan, number, stats, result.error)) return result;
        // recorded only once the device confirmed the copy
        recorded->second.patch = number;
        recorded->second.content = manifest.files;
        std::string error;
        if (!records.save(request.internalDir, error)) Out::warning("%s", error.c_str());
        result.installed = "patched";
        result.pushedFiles = stats.files;
        result.pushedBytes = stats.bytes;
    } else {
        say("Installing on " + device.name);
        if (!runner.install(run, result.error)) return result;
        result.installed = "full";
        if (mobile) {
            DeviceRecord record;
            record.name = device.name;
            record.platform = buildPlatformName(device.platform);
            record.package = run.appId;
            std::string sha;
            if (fs::is_regular_file(run.package)) sha256File(run.package, sha);
            record.packageSha256 = sha;
            record.playerId = request.playerId;
            record.playerSha256 = request.playerIdentity;
            record.development = run.development;
            record.debuggable = request.debuggable;
            record.patch = 0;
            record.content = manifest.files;
            records.devices[device.id] = record;
            std::string error;
            if (!records.save(request.internalDir, error)) Out::warning("%s", error.c_str());
        }
    }

    say("Launching on " + device.name);
    const auto launchStart = std::chrono::steady_clock::now();
    if (!runner.launch(run, result.error)) return result;
    result.launchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - launchStart).count();
    result.url = runner.state().url;
    result.ok = true;
    return result;
}

std::vector<DeviceRuns::Run> DeviceRuns::runs() const {
    std::vector<std::shared_ptr<Entry>> list;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [id, entry] : entries) list.push_back(entry);
    }
    std::vector<Run> out;
    for (const auto& entry : list) out.push_back({entry->device, entry->runner->state()});
    return out;
}

std::optional<DeviceRunState> DeviceRuns::state(const std::string& deviceId) const {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(deviceId);
        if (it == entries.end()) return std::nullopt;
        entry = it->second;
    }
    return entry->runner->state();
}

bool DeviceRuns::stop(const std::string& deviceId) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(deviceId);
        if (it == entries.end()) return false;
        entry = it->second;
    }
    entry->runner->stop();
    return true;
}

bool DeviceRuns::screenshot(const std::string& deviceId, std::vector<uint8_t>& png, std::string& error) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(deviceId);
        if (it == entries.end()) {
            error = "nothing runs on " + deviceId;
            return false;
        }
        entry = it->second;
    }
    return entry->runner->screenshot(png, error);
}

bool DeviceRuns::saveScreenshot(const std::string& deviceId, fs::path& saved, std::string& error) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(deviceId);
        if (it == entries.end()) {
            error = "nothing runs on " + deviceId;
            return false;
        }
        entry = it->second;
    }
    std::vector<uint8_t> png;
    if (!entry->runner->screenshot(png, error)) return false;
    std::string name = entry->device.name;
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '-';
    // Recordings/ is not an asset folder: the screenshots never import
    saved = entry->projectDir / "Recordings" / "Device" / (name + "-" + deviceUtcNow(true) + ".png");
    safewrite::EditorInternal own(saved.parent_path());
    const io::WriteResult written = io::writeFileAtomic(saved, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    if (!written.ok()) {
        error = written.message;
        return false;
    }
    return true;
}

void DeviceRuns::dismiss(const std::string& deviceId) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(deviceId);
        if (it == entries.end() || it->second->runner->state().running) return;
        entry = it->second;
        entries.erase(it);
    }
}

void DeviceRuns::releaseAll() {
    std::map<std::string, std::shared_ptr<Entry>> all;
    {
        std::lock_guard<std::mutex> lock(mutex);
        all.swap(entries);
    }
    for (auto& [id, entry] : all) entry->runner->release();
}

void DeviceRuns::stopAll() {
    std::map<std::string, std::shared_ptr<Entry>> all;
    {
        std::lock_guard<std::mutex> lock(mutex);
        all.swap(entries);
    }
    for (auto& [id, entry] : all) entry->runner->stop();
}

}  // namespace jasperengine::editor::platform
