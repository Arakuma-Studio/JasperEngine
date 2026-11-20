// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S28: crash and hang diagnostics (docs/crash-diagnostics.md), tool group "diagnostics"
//
//   get_editor_health      responsive or hung, answered off the main thread
//   get_crash_reports      the report folders (crash, hang, abnormal exit, user) and the startup notice;
//                          a game's reports root (folder), kinds, files to inline, group (util/CrashGroups)
//   dismiss_crash_notice   the notice's Dismiss: writes `handled`
//   set_hang_watchdog      Preferences > Diagnostics
//   debug_crash            crash or hang the editor on purpose (--allow-debug-crash only)

#include "McpTools.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "diag/CrashHandler.h"
#include "diag/HangWatchdog.h"
#include "mcp/McpCall.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "util/CrashGroups.h"
#include "util/EditorDiagnostics.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
namespace fs = std::filesystem;

Json scopeArray(const std::vector<std::string>& scopes) {
    Json out = Json::array();
    for (const std::string& scope : scopes) out.push_back(scope);
    return out;
}

void getEditorHealthTool(const CallPtr& call) {
    // any thread: the watchdog's atomics, nothing the main thread holds
    const diag::HealthSnapshot health = diag::watchdog::health();
    Json data = {
        {"responsive", health.responsive},
        {"stalled_ms", health.stalledMs},
        {"phase", health.phase},
        {"busy_scopes", scopeArray(health.busyScopes)},
        {"hang_reports", health.reports},
        {"shutting_down", health.shutdownWindow},
        {"watchdog", {{"enabled", health.enabled}, {"threshold_ms", health.thresholdMs}}},
    };
    std::string text = health.responsive ? "The editor is responsive" : "The editor is NOT responding";
    if (health.stalledMs > 0) text += " (main thread quiet for " + std::to_string(health.stalledMs) + " ms)";
    if (!health.busyScopes.empty()) text += "; busy: " + health.busyScopes.back();
    if (!health.enabled) text += ". The hang watchdog is off, so this is not measured.";
    call->ok(text + ".", data);
}

std::string readText(const fs::path& file, size_t limit) {
    std::ifstream in(file, std::ios::binary);
    std::string text(limit, '\0');
    in.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<size_t>(in.gcount()));
    return text;
}

// R37: a game's reports root holds the report folders, or (as pulled from a device) reports/ does
std::optional<fs::path> reportsFolderAt(const fs::path& path) {
    if (!diag::crash::listReportsIn(path, 1).empty()) return path;
    if (!diag::crash::listReportsIn(path / "reports", 1).empty()) return path / "reports";
    return std::nullopt;
}

// a file's text for `files`: at most 64 KiB, a binary file only named
Json inlinedText(const fs::path& file) {
    std::string text = readText(file, 64 * 1024);
    if (text.find('\0') != std::string::npos) {
        std::error_code ec;
        return "(binary, " + std::to_string(fs::file_size(file, ec)) + " bytes: read it from folder)";
    }
    return StringUtils::repairUtf8(text);
}

void getCrashReportsTool(const CallPtr& call) {
    const Json& args = call->args();
    const size_t limit = static_cast<size_t>(args.value("limit", 10));
    const bool includeText = args.value("include_text", false);
    const bool grouped = args.value("group", false);
    fs::path folder = diag::crash::reportsDirectory();
    const bool foreign = args.contains("folder");
    if (foreign) {
        const fs::path given = fs::path(args["folder"].get<std::string>());
        const std::optional<fs::path> found = given.is_absolute() ? reportsFolderAt(given) : std::nullopt;
        if (!found) {
            call->fail("No report folder at " + given.string() + ": give a reports root (it holds <yyyymmddThhmmssZ>-<pid> folders).");
            return;
        }
        folder = *found;
    }
    std::vector<std::string> kinds;
    for (const Json& kind : args.value("kinds", Json::array())) kinds.push_back(kind.get<std::string>());
    std::vector<std::string> files;
    for (const Json& name : args.value("files", Json::array())) {
        const std::string file = name.get<std::string>();
        if (file.empty() || file == "." || file == ".." || file.find_first_of("/\\") != std::string::npos) {
            call->fail("files: " + file + " is not a file name; give names inside a report folder, as journal.txt.");
            return;
        }
        files.push_back(file);
    }

    // a filter or groups need every report; a plain listing reads only the newest `limit`
    std::vector<diag::crash::ReportInfo> listed = diag::crash::listReportsIn(folder, kinds.empty() && !grouped ? limit : 0);
    if (!kinds.empty()) {
        listed.erase(std::remove_if(listed.begin(), listed.end(), [&](const diag::crash::ReportInfo& r) {
            return std::find(kinds.begin(), kinds.end(), r.kind) == kinds.end();
        }), listed.end());
    }
    const std::optional<diag::crash::ReportInfo> notice = EditorDiagnostics::notice();
    Json data = {
        {"folder", folder.string()},
        {"session", diag::crash::sessionId()},
        {"notice_open", notice.has_value()},
    };
    if (notice) data["notice"] = {{"id", notice->id}, {"text", EditorDiagnostics::noticeText(*notice)}};
    std::string text;
    if (grouped) {
        std::map<int64_t, std::string> isoOf;
        for (const diag::crash::ReportInfo& report : listed) isoOf[report.timeUnix] = report.time;
        Json groups = Json::array();
        bool unresolved = false;
        for (const crashgroups::Group& g : crashgroups::group(listed)) {
            if (groups.size() >= limit) break;
            Json versions = Json::object(), platforms = Json::object();
            for (const auto& [name, count] : g.versions) versions[name] = count;
            for (const auto& [name, count] : g.platforms) platforms[name] = count;
            // the newest 20: count says how many there are
            std::vector<std::string> ids(g.reportIds.begin(), g.reportIds.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(g.reportIds.size(), 20)));
            groups.push_back({{"signature", g.signature}, {"kind", g.kind}, {"count", g.count}, {"versions", versions},
                              {"platforms", platforms}, {"first_seen", isoOf[g.firstSeen]}, {"last_seen", isoOf[g.lastSeen]},
                              {"report_ids", ids}, {"unresolved", g.unresolved}});
            unresolved = unresolved || g.unresolved;
        }
        data["groups"] = groups;
        data["reports_read"] = listed.size();
        if (unresolved) data["next"] = "Names need symbols: python3 -I tools/crash/symbolicate.py --group " + folder.string() + " --symbols <dir>";
        text = std::to_string(groups.size()) + (groups.size() == 1 ? " group" : " groups") + " of " + std::to_string(listed.size()) +
               (listed.size() == 1 ? " report" : " reports") + " in " + folder.string();
    } else {
        if (listed.size() > limit) listed.resize(limit);
        Json reports = Json::array();
        for (const diag::crash::ReportInfo& report : listed) {
            Json names = Json::array();
            for (const fs::path& file : report.files) names.push_back(file.filename().string());
            Json entry = {
                {"id", report.id},
                {"kind", report.kind},
                {"time", report.time},
                {"reason", report.reason},
                {"product", report.product},
                {"version", report.version},
                {"folder", report.folder.string()},
                {"files", names},
                {"handled", report.handled},
                {"this_session", !foreign && report.id == diag::crash::sessionId()},
            };
            if (includeText) {
                const fs::path primary = EditorDiagnostics::primaryFile(report);
                if (primary != report.folder) entry["text"] = readText(primary, 64 * 1024);
            }
            if (!files.empty()) {
                Json texts = Json::object();
                for (const std::string& name : files) {
                    std::error_code ec;
                    if (fs::is_regular_file(report.folder / name, ec)) texts[name] = inlinedText(report.folder / name);
                }
                entry["texts"] = texts;
            }
            reports.push_back(std::move(entry));
        }
        data["reports"] = reports;
        text = std::to_string(reports.size()) + (reports.size() == 1 ? " report" : " reports") + " in " + folder.string();
    }
    if (notice) text += "; the startup notice is open (" + notice->id + ")";
    call->ok(text + ".", data);
}

void dismissCrashNoticeTool(const CallPtr& call) {
    const Json& args = call->args();
    std::string id = args.value("id", std::string());
    if (id.empty()) {
        const std::optional<diag::crash::ReportInfo> notice = EditorDiagnostics::notice();
        if (!notice) {
            call->fail("No crash notice is open; give the id of a report (get_crash_reports) to mark it handled.");
            return;
        }
        id = notice->id;
    }
    if (!EditorDiagnostics::dismiss(id)) {
        call->fail("No report " + id + " (get_crash_reports lists them).");
        return;
    }
    call->ok("Report " + id + " is handled; the notice is closed.", Json{{"id", id}, {"notice_open", EditorDiagnostics::isNoticeOpen()}});
}

void setHangWatchdogTool(const CallPtr& call) {
    const Json& args = call->args();
    DiagnosticsSettings settings = AppSettings::getDiagnosticsSettings();
    if (args.contains("enabled")) settings.hangWatchdog = args["enabled"].get<bool>();
    if (args.contains("threshold_ms")) {
        settings.reportAfterSeconds = static_cast<int>((args["threshold_ms"].get<int64_t>() + 500) / 1000);
    }
    if (args.contains("break_lua_after_s")) settings.breakLuaAfterSeconds = args["break_lua_after_s"].get<int>();
    AppSettings::setDiagnosticsSettings(settings);
    AppSettings::saveSettings();
    const DiagnosticsSettings applied = AppSettings::getDiagnosticsSettings();
    Json data = {
        {"enabled", applied.hangWatchdog},
        {"threshold_ms", applied.reportAfterSeconds * 1000},
        {"break_lua_after_s", applied.breakLuaAfterSeconds},
        {"running", diag::watchdog::isRunning()},
    };
    call->ok(std::string("Hang watchdog ") + (applied.hangWatchdog ? "on, report after " + std::to_string(applied.reportAfterSeconds) + " s" : "off") +
             ", runaway Lua in Play interrupted after " +
             (applied.breakLuaAfterSeconds > 0 ? std::to_string(applied.breakLuaAfterSeconds) + " s" : std::string("never")) + ".",
             data);
}

void debugCrashTool(const CallPtr& call) {
    if (!EditorDiagnostics::isDebugCrashAllowed()) {
        call->fail("debug_crash is refused: start the editor with --allow-debug-crash (or JASPERENGINE_ALLOW_DEBUG_CRASH=1).");
        return;
    }
    const Json& args = call->args();
    const std::string mode = args.value("mode", std::string());
    const uint32_t hangMs = static_cast<uint32_t>(args.value("hang_ms", 6000));
    if (!diag::crash::isDebugCrashMode(mode)) {
        call->fail("Unknown mode " + mode + ".");
        return;
    }
    // the answer goes out first; the main thread hangs or crashes on its next turn
    call->ok(mode == "hang" ? "The main thread hangs for " + std::to_string(hangMs) + " ms from its next turn."
                            : "The editor crashes (" + mode + ") on the main thread's next turn.",
             Json{{"mode", mode}, {"hang_ms", hangMs}});
    getEditorHost().enqueueMainThreadTask([mode, hangMs]() {
        if (mode == "hang") {
            const diag::PhaseScope phase("debug_crash:hang");
            diag::crash::debugCrash(mode, hangMs);
            return;
        }
        // let the reply reach the client before the process goes
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        diag::crash::debugCrash(mode);
    });
}

} // namespace

void registerDiagnosticsTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "get_editor_health";
        tool.title = "Get Editor Health";
        tool.description =
            "Whether the editor's main thread is responsive, answered off the main thread: responsive, stalled_ms, "
            "phase, busy_scopes (known long work), watchdog. Call it when other tools time out, to tell hung from slow.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.offMainThread = true;
        tool.handler = getEditorHealthTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_crash_reports";
        tool.title = "Get Crash Reports";
        tool.description =
            "Crash, hang, abnormal_exit and user report folders of this editor, newest first ({id, kind, time, reason, "
            "files, handled}), and notice_open (the startup notice). include_text adds crash.txt or the hang report. "
            "folder reads a game's reports; group: true merges similar crashes with counts per version and platform.";
        tool.inputSchema = object({
            {"limit", withDefault(integer("", 1, 100), 10)},
            {"include_text", withDefault(boolean(""), false)},
            {"folder", string("Absolute path of a reports root. Default: this editor's.")},
            {"kinds", array(stringEnum("", {"crash", "hang", "abnormal_exit", "user"}), "")},
            {"group", withDefault(boolean(""), false)},
            {"files", array(string(""), "File names to inline as text, 64 KiB each.")},
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getCrashReportsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "dismiss_crash_notice";
        tool.title = "Dismiss Crash Notice";
        tool.description = "The startup notice's Dismiss: marks the report handled (default: the notice's) so it is not shown again.";
        tool.inputSchema = object({{"id", string("A report id (get_crash_reports).")}});
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Control;
        tool.handler = dismissCrashNoticeTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_hang_watchdog";
        tool.title = "Set Hang Watchdog";
        tool.description =
            "Preferences > Diagnostics, saved at once (not undoable, like other preferences): the hang watchdog on or "
            "off, its threshold (whole seconds) and the runaway-Lua limit in Play (0 = never).";
        tool.inputSchema = object({
            {"enabled", boolean("")},
            {"threshold_ms", integer("", 1000, 60000)},
            {"break_lua_after_s", integer("", 0, 120)},
        });
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Control;
        tool.handler = setHangWatchdogTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_crash";
        tool.title = "Debug Crash";
        tool.description =
            "Test the diagnostics: crash the editor, or hang its main thread for hang_ms. Refused unless it was "
            "started with --allow-debug-crash. Replies first, then acts on the main thread's next turn.";
        tool.inputSchema = object({
            {"mode", stringEnum("", {"segv", "abort", "terminate", "stack_overflow", "worker_segv", "hang"})},
            {"hang_ms", withDefault(integer("", 100, 120000), 6000)},
        }, {"mode"});
        tool.annotations.destructive = true;
        tool.activity = ToolActivity::Control;
        tool.handler = debugCrashTool;
        registry.addTool(std::move(tool));
    }
}

} // namespace jasperengine::editor::mcp
