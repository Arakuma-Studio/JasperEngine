// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorDiagnostics.h"

#include "EditorVersion.h"
#include "Out.h"
#include "diag/HangWatchdog.h"
#include "script/LuaWatchdog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct State {
        std::mutex mutex;
        bool installed = false;
        bool debugCrashAllowed = false;
        bool playing = false;
        DiagnosticsSettings settings;
        std::optional<diag::crash::ReportInfo> notice;
        std::optional<EditorDiagnostics::Toast> toast;
        std::chrono::steady_clock::time_point toastShown{};
        std::chrono::steady_clock::time_point lastToast{};
    };

    State& state() {
        static State instance;
        return instance;
    }

    std::string localClock(int64_t unixTime) {
        const std::time_t time = static_cast<std::time_t>(unixTime);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        char text[16];
        std::snprintf(text, sizeof(text), "%02d:%02d", tm.tm_hour, tm.tm_min);
        return text;
    }

    void startWatchdog(const DiagnosticsSettings& settings) {
        if (!settings.hangWatchdog) {
            diag::watchdog::stop();
            return;
        }
        if (diag::watchdog::isRunning()) {
            diag::watchdog::setThresholdMs(static_cast<uint32_t>(settings.reportAfterSeconds) * 1000u);
            return;
        }
        diag::WatchdogConfig config;
        config.thresholdMs = static_cast<uint32_t>(settings.reportAfterSeconds) * 1000u;
        config.product = "JasperEngine Editor";
        config.version = JASPERENGINE_EDITOR_VERSION;
        diag::watchdog::start(config);
        LuaWatchdog::attach();
    }

    void applyPlayLimit(const State& s) {
        diag::watchdog::setBreakLuaAfterMs(s.playing ? static_cast<uint32_t>(s.settings.breakLuaAfterSeconds) * 1000u : 0u);
    }

}

void EditorDiagnostics::install(const fs::path& configDirectory) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.installed) return;
    s.installed = true;
    if (const char* allow = std::getenv("JASPERENGINE_ALLOW_DEBUG_CRASH"); allow && std::strcmp(allow, "1") == 0) {
        s.debugCrashAllowed = true;
    }

    diag::crash::Config config = diag::crash::editorConfig(configDirectory, JASPERENGINE_EDITOR_VERSION);
    config.logFile = configDirectory / "editor.log";
#if defined(SOKOL_VULKAN)
    config.backend = "vulkan";
#elif defined(SOKOL_METAL)
    config.backend = "metal";
#elif defined(SOKOL_D3D11)
    config.backend = "d3d11";
#else
    config.backend = "glcore";
#endif
    diag::crash::install(config);

    if (s.settings.showCrashNotice) {
        s.notice = diag::crash::lastSessionReport();
        if (s.notice) {
            Out::warning("%s Report: %s", noticeText(*s.notice).c_str(), primaryFile(*s.notice).string().c_str());
        }
    }
    startWatchdog(s.settings);
    applyPlayLimit(s);
}

void EditorDiagnostics::applySettings(const DiagnosticsSettings& settings) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.settings = settings;
    if (!s.installed) return;
    startWatchdog(settings);
    applyPlayLimit(s);
}

void EditorDiagnostics::beginShutdown() {
    diag::watchdog::beginShutdownWatch();
}

void EditorDiagnostics::end() {
    diag::watchdog::stop();
    diag::crash::shutdown();
}

void EditorDiagnostics::setPlaying(bool playing, const std::string& sceneName) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.playing = playing;
    applyPlayLimit(s);
    diag::watchdog::setContext(diag::ContextSlot::PlayState, playing ? "playing" : "editing");
    diag::watchdog::setContext(diag::ContextSlot::Scene, sceneName);
}

void EditorDiagnostics::setDebugCrashAllowed(bool allowed) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.debugCrashAllowed = allowed;
}

bool EditorDiagnostics::isDebugCrashAllowed() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.debugCrashAllowed;
}

std::optional<diag::crash::ReportInfo> EditorDiagnostics::notice() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.notice;
}

bool EditorDiagnostics::isNoticeOpen() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.notice.has_value();
}

bool EditorDiagnostics::dismiss(const std::string& id) {
    if (!diag::crash::markHandled(id)) return false;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.notice && s.notice->id == id) s.notice.reset();
    return true;
}

std::string EditorDiagnostics::noticeText(const diag::crash::ReportInfo& report) {
    const std::string when = localClock(report.timeUnix);
    if (report.kind == "crash") {
        const std::string reason = report.reason.substr(0, report.reason.find(' '));
        return "JasperEngine closed unexpectedly last time (" + (reason.empty() ? std::string("crash") : reason) + " at " +
               when + "). A crash report was saved.";
    }
    if (report.kind == "hang") {
        return "JasperEngine closed unexpectedly last time (at " + when + ") while it was not responding (hang report saved).";
    }
    return "JasperEngine closed unexpectedly last time (at " + when + "); no report was saved. editor.log may say more.";
}

void EditorDiagnostics::pollNotices() {
    const std::vector<diag::HangNotice> notices = diag::watchdog::takeNotices();
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    for (const diag::HangNotice& notice : notices) {
        const std::string text = hangToastText(notice.stalledMs, notice.label);
        Out::warning("%s%s%s", text.c_str(), notice.file.empty() ? "" : " ", notice.file.string().c_str());
        if (s.lastToast.time_since_epoch().count() == 0 || now - s.lastToast >= std::chrono::seconds(60)) {
            s.toast = Toast{text, notice.file};
            s.toastShown = now;
            s.lastToast = now;
        }
    }
    if (s.toast && now - s.toastShown >= std::chrono::seconds(8)) s.toast.reset();
}

std::optional<EditorDiagnostics::Toast> EditorDiagnostics::activeToast() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.toast;
}

void EditorDiagnostics::closeToast() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.toast.reset();
}

std::string EditorDiagnostics::hangToastText(uint64_t stalledMs, const std::string& label) {
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", static_cast<double>(stalledMs) / 1000.0);
    return std::string("The editor was not responding for ") + seconds + " s" +
           (label.empty() || label == "unknown" ? std::string() : " (" + label + ")") + ". Details saved.";
}

fs::path EditorDiagnostics::primaryFile(const diag::crash::ReportInfo& report) {
    fs::path best;
    for (const fs::path& file : report.files) {
        const std::string name = file.filename().string();
        if (name == "crash.txt") return file;
        if (name.rfind("hang-", 0) == 0 && file.extension() == ".txt") best = file;
        if (best.empty() && name == "session.json") best = file;
    }
    return best.empty() ? report.folder : best;
}
