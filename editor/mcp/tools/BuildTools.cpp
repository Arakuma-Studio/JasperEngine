// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W5: async export/build/run players
//
//   get_build_info     Build Settings state: modes, tools, output dirs, graphics APIs, shaders
//   build_project      export / build a player as a job (replaces export_project)
//   generate_shaders   compile shader sets as a job (replaces the legacy blocking one)
//   run_build          launch a built Desktop player, capture its log and a screenshot, stop it
//
// Builds never block the main thread: Exporter::startExport runs on its own thread (it hops to
// the main thread to save scenes, which keeps running since tool handlers return at once) and a
// watcher thread forwards its progress and log lines (Out) into the McpJob.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "AppSettings.h"
#include "Exporter.h"
#include "Factory.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "pool/ShaderPool.h"
#include "util/Base64.h"

#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ X11 capture (defined at the end of the file)

struct ScreenCapture {
    bool ok = false;
    std::vector<uint8_t> png;
    int width = 0;
    int height = 0;
    std::string note;
};

// Top-level windows of the display (stacking order), empty without X11
std::vector<unsigned long> listTopLevelWindows();
// The player's window: the viewable one whose _NET_WM_PID is pid, else the topmost new one
// (not in `before`); 0 when there is none (yet)
unsigned long findPlayerWindow(long pid, const std::vector<unsigned long>& before);
// The window read from the screen (window 0: the whole screen). maxWidth scales the image down.
ScreenCapture captureWindow(unsigned long window, int maxWidth);

// ------------------------------------------------------------------ player input (X11, at the end)

// One timed input action of run_build's input script
struct PlayerAction {
    enum Kind { KeyDown, KeyUp, ButtonDown, ButtonUp, Move, ReleaseAll, Screenshot };
    Kind kind = Move;
    double at = 0.0;               // seconds after the input starts
    unsigned long keysym = 0;
    int button = 1;                // X buttons: 1 left, 2 middle, 3 right, 4-7 scroll
    double x = 0.0, y = 0.0;       // window pixels, or 0..1 when normalized
    bool normalized = false;
};

// Why input can't reach a player here ("" = it can)
std::string playerInputUnsupported();
// X keysym of a key name (space, a, 0, left, enter, f1, shift, kp_0...), 0 if unknown
unsigned long playerKeysym(const std::string& name);
// X keysym of a character, with shift when it needs it; 0 if it has none
unsigned long playerCharKeysym(uint32_t codepoint, bool& shift);
// A connection sending input (XTest, else XSendEvent) to one window; null if none
struct PlayerInputSender;
PlayerInputSender* openPlayerInput(std::string& method, std::string& error);
// "" when sent
std::string sendPlayerAction(PlayerInputSender* sender, unsigned long window, const PlayerAction& action);
// Releases what is still held and closes the connection
void closePlayerInput(PlayerInputSender* sender, unsigned long window);
// {x, y, width, height} of the window on the screen
bool playerWindowRect(unsigned long window, int& x, int& y, int& width, int& height);

// ------------------------------------------------------------------ helpers

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const char* modeName(ExportMode mode) {
    switch (mode) {
        case ExportMode::SourceCode: return "source";
        case ExportMode::Desktop: return "desktop";
        case ExportMode::Web: return "web";
    }
    return "source";
}

// AppSettings key of the remembered output dir (ExportWindow::localExportMode)
const char* settingsModeKey(ExportMode mode) {
    return mode == ExportMode::SourceCode ? "sourceCode" : mode == ExportMode::Desktop ? "desktop" : "web";
}

const char* hostPlatformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// Graphics APIs a Desktop player of this host can use (ExportWindow desktopGraphicBackends)
std::vector<ShaderBackend> desktopBackends() {
#if defined(_WIN32)
    return {ShaderBackend::GLCore, ShaderBackend::Vulkan, ShaderBackend::D3D11};
#elif defined(__APPLE__)
    return {ShaderBackend::MetalMacOS, ShaderBackend::GLCore};
#else
    return {ShaderBackend::GLCore, ShaderBackend::Vulkan};
#endif
}

// Source Code presets per target platform (ExportWindow::platformBackends)
bool platformBackends(const std::string& platform, std::set<ShaderBackend>& out) {
    if (platform == "windows") out = {ShaderBackend::GLCore, ShaderBackend::Vulkan, ShaderBackend::D3D11};
    else if (platform == "macos") out = {ShaderBackend::MetalMacOS, ShaderBackend::GLCore};
    else if (platform == "linux") out = {ShaderBackend::GLCore, ShaderBackend::Vulkan};
    else if (platform == "android") out = {ShaderBackend::GLES3, ShaderBackend::Vulkan};
    else if (platform == "ios") out = {ShaderBackend::MetalIOS};
    else if (platform == "web") out = {ShaderBackend::GLES3};
    else return false;
    return true;
}

std::string backendToken(ShaderBackend backend) {
    return ShaderPool::getShaderBackendCliToken(backend);
}

Json backendList(const std::set<ShaderBackend>& backends) {
    Json list = Json::array();
    for (ShaderBackend b : backends) list.push_back(backendToken(b));
    return list;
}

std::string allBackendTokens() {
    std::string text;
    for (ShaderBackend b : ShaderPool::getShaderBackends()) text += (text.empty() ? "" : ", ") + backendToken(b);
    return text;
}

bool parseBackends(const Json& value, std::set<ShaderBackend>& out, std::string& error) {
    std::vector<std::string> tokens;
    if (value.is_string()) {
        std::stringstream ss(value.get<std::string>());
        std::string token;
        while (std::getline(ss, token, ',')) tokens.push_back(token);
    } else if (value.is_array()) {
        for (const Json& item : value) {
            if (!item.is_string()) {
                error = "backends must be strings";
                return false;
            }
            tokens.push_back(item.get<std::string>());
        }
    } else {
        error = "backends must be a list of graphics API names";
        return false;
    }
    for (std::string token : tokens) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;
        if (lowerText(token) == "all") {
            for (ShaderBackend b : ShaderPool::getShaderBackends()) out.insert(b);
            continue;
        }
        ShaderBackend backend;
        if (!ShaderPool::parseShaderBackend(token, backend)) {
            error = "unknown graphics API \"" + token + "\"; use " + allBackendTokens() + " or all";
            return false;
        }
        out.insert(backend);
    }
    if (out.empty()) {
        error = "no graphics API given; use " + allBackendTokens() + " or all";
        return false;
    }
    return true;
}

std::string shaderKeyText(ShaderKey key) {
    std::ostringstream text;
    text << "0x" << std::hex << key;
    return text.str();
}

bool parseShaderKey(const Json& value, ShaderKey& out) {
    try {
        if (value.is_number_unsigned()) {
            out = value.get<uint64_t>();
            return true;
        }
        if (value.is_number_integer() && value.get<long long>() >= 0) {
            out = static_cast<ShaderKey>(value.get<long long>());
            return true;
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            size_t used = 0;
            out = static_cast<ShaderKey>(std::stoull(text, &used, 0));
            return used == text.size();
        }
    } catch (...) {
    }
    return false;
}

std::string shaderName(ShaderKey key) {
    return Exporter::getShaderDisplayName(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key),
                                          ShaderPool::getCustomIdFromKey(key));
}

// The shaders the scenes need (ExportWindow::populateShaderList): stored keys plus a live collection
std::set<ShaderKey> sceneShaderKeys(Project* project) {
    std::set<ShaderKey> keys;
    for (const SceneProject& scene : project->getScenes()) {
        std::set<ShaderKey> sceneKeys = scene.shaderKeys;
        project->collectSceneShaderKeys(&scene, sceneKeys);
        project->collectRequiredShaderKeys(&scene, sceneKeys);
        for (ShaderKey key : sceneKeys) keys.insert(ShaderPool::normalizeKey(key));
    }
    return keys;
}

std::string appName(Project* project) {
    return project->getName().empty() ? "jasperengine-project" : Factory::toIdentifier(project->getName());
}

std::string executableName(Project* project) {
#ifdef _WIN32
    return appName(project) + ".exe";
#else
    return appName(project);
#endif
}

bool isInside(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(path, ec);
    const fs::path b = fs::weakly_canonical(root, ec);
    auto ai = a.begin();
    for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
        if (bi->empty()) continue;
        if (ai == a.end() || *ai != *bi) return false;
    }
    return true;
}

fs::path defaultOutputDir(Project* project, ExportMode mode) {
    const fs::path file = project->getProjectPath() / "project.yaml";
    fs::path dir = AppSettings::getExportTargetDir(file, settingsModeKey(mode));
    if (!dir.empty()) return dir;
    const fs::path base = AppSettings::getDefaultExportDirectory();
    if (!base.empty()) return base / (appName(project) + "-" + modeName(mode));
    return project->getProjectInternalPath() / "builds" / modeName(mode);
}

std::string stripAnsi(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) i++;
            continue;
        }
        out += line[i];
    }
    return out;
}

// ------------------------------------------------------------------ compiler diagnostics

struct Diagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string severity;   // error, warning
    std::string code;
    std::string message;
};

// file:line:col: error: message (GCC/Clang), file(line,col): error C1234: message (MSVC),
// CMake Error at file:line, linker errors
bool parseDiagnostic(const std::string& rawLine, Diagnostic& out) {
    const std::string line = stripAnsi(rawLine);
    if (line.find("error") == std::string::npos && line.find("warning") == std::string::npos &&
        line.find("Error") == std::string::npos && line.find("undefined reference") == std::string::npos) {
        return false;
    }
    static const std::regex gcc(R"(^\s*(.+?):(\d+):(?:(\d+):)?\s+(fatal error|error|warning):\s+(.*)$)");
    static const std::regex msvc(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s+(fatal error|error|warning)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)");
    static const std::regex cmake(R"(^\s*CMake (Error|Warning)(?: \(dev\))? at (.+?):(\d+)(.*)$)");
    static const std::regex linker(R"((undefined reference to .*|multiple definition of .*|ld(?:\.\w+)?: (?:error|fatal error): .*|collect2: error: .*|LINK : fatal error .*))");
    std::smatch m;
    if (std::regex_match(line, m, gcc)) {
        out.file = m[1];
        out.line = std::stoi(m[2]);
        out.column = m[3].matched ? std::stoi(m[3]) : 0;
        out.severity = m[4] == "warning" ? "warning" : "error";
        out.message = m[5];
        return out.file.find(' ') == std::string::npos || out.file.find('/') != std::string::npos ||
               out.file.find('\\') != std::string::npos;
    }
    if (std::regex_match(line, m, msvc)) {
        out.file = m[1];
        out.line = std::stoi(m[2]);
        out.column = m[3].matched ? std::stoi(m[3]) : 0;
        out.severity = m[4] == "warning" ? "warning" : "error";
        out.code = m[5];
        out.message = m[6];
        return true;
    }
    if (std::regex_match(line, m, cmake)) {
        out.file = m[2];
        out.line = std::stoi(m[3]);
        out.severity = m[1] == "Warning" ? "warning" : "error";
        out.code = "cmake";
        out.message = "CMake " + std::string(m[1]) + std::string(m[4]);
        return true;
    }
    if (std::regex_search(line, m, linker)) {
        out.severity = "error";
        out.code = "link";
        out.message = line;
        return true;
    }
    return false;
}

// The project file a build-tree path was copied from: the longest trailing part of the path
// that exists in the project
std::string projectFileFor(const fs::path& projectPath, const std::string& file) {
    if (file.empty()) return {};
    const fs::path path = fs::path(file).lexically_normal();
    std::vector<fs::path> parts(path.begin(), path.end());
    std::error_code ec;
    for (size_t start = 0; start < parts.size(); start++) {
        fs::path suffix;
        for (size_t i = start; i < parts.size(); i++) suffix /= parts[i];
        if (suffix.is_absolute() || suffix.has_root_name()) continue;
        // the build cache (.jasperengine/export/...) holds copies: the source is elsewhere
        if (*suffix.begin() == ".jasperengine") continue;
        if (parts.size() - start >= 1 && fs::is_regular_file(projectPath / suffix, ec)) {
            if (parts.size() - start == 1 && start != parts.size() - 1) continue;
            return suffix.generic_string();
        }
    }
    return {};
}

// ------------------------------------------------------------------ build runs

struct BuildRun {
    JobPtr job;
    ExportMode mode = ExportMode::SourceCode;
    bool shadersOnly = false;
    ExportConfig config;
    fs::path projectPath;
    std::string executableName;
    std::mutex mutex;                     // guards exporter
    std::unique_ptr<Exporter> exporter;
    std::thread worker;                   // generate_shaders: runs Exporter::generateShaders
    uint64_t logCursor = 0;
    std::vector<Diagnostic> diagnostics;
    size_t errorCount = 0;
    size_t warningCount = 0;
    std::ofstream logFile;
    fs::path logPath;
    std::string lastError;                // last [error] line, for the failure message
};

// Never destroyed: a build still running at exit must not be joined from a static destructor
// (its thread may wait for the main thread, which no longer runs)
struct BuildManager {
    std::mutex mutex;
    std::shared_ptr<BuildRun> active;
    Json lastBuild = nullptr;             // the last finished build's result
    fs::path lastExecutable;              // the last Desktop player built
};

BuildManager& manager() {
    static BuildManager* instance = new BuildManager();
    return *instance;
}

void consumeLog(BuildRun& run, const LogEntry& entry) {
    std::string prefix;
    switch (entry.type) {
        case LogType::Error: prefix = "[error] "; break;
        case LogType::Warning: prefix = "[warning] "; break;
        default: break;
    }
    // one entry may carry several lines
    std::stringstream ss(entry.message);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        run.job->log(prefix + line);
        if (run.logFile) run.logFile << prefix << line << "\n";
        if (entry.type == LogType::Error) run.lastError = line;
        Diagnostic d;
        if (parseDiagnostic(line, d)) {
            if (d.severity == "error") run.errorCount++;
            else run.warningCount++;
            const bool duplicate = std::any_of(run.diagnostics.begin(), run.diagnostics.end(), [&d](const Diagnostic& o) {
                return o.file == d.file && o.line == d.line && o.message == d.message;
            });
            const size_t errorsKept = static_cast<size_t>(std::count_if(run.diagnostics.begin(), run.diagnostics.end(),
                [](const Diagnostic& o) { return o.severity == "error"; }));
            if (!duplicate && ((d.severity == "error" && errorsKept < 50) || (d.severity == "warning" && run.diagnostics.size() - errorsKept < 20))) {
                run.diagnostics.push_back(d);
            }
        }
    }
}

Json diagnosticsJson(const BuildRun& run, const std::string& severity) {
    Json list = Json::array();
    for (const Diagnostic& d : run.diagnostics) {
        if (d.severity != severity) continue;
        Json item = {{"file", d.file}, {"line", d.line}, {"message", d.message}};
        if (d.column > 0) item["column"] = d.column;
        if (!d.code.empty()) item["code"] = d.code;
        const std::string projectFile = projectFileFor(run.projectPath, d.file);
        if (!projectFile.empty()) item["project_file"] = projectFile;
        list.push_back(item);
    }
    return list;
}

Json artifactsJson(const fs::path& dir) {
    Json list = Json::array();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return list;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string() + (entry.is_directory(ec) ? "/" : ""));
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) list.push_back(name);
    return list;
}

void finishRun(const std::shared_ptr<BuildRun>& run, const ExportProgress& progress) {
    BuildRun& r = *run;
    Json result = {
        {"mode", r.shadersOnly ? "shaders" : modeName(r.mode)},
        {"errors", diagnosticsJson(r, "error")},
        {"warnings", diagnosticsJson(r, "warning")},
        {"error_count", r.errorCount},
        {"warning_count", r.warningCount},
        {"log_file", r.logPath.generic_string()}
    };
    const fs::path outputDir = r.shadersOnly || r.mode == ExportMode::SourceCode ? r.config.targetDir : r.config.destinationDir;
    result["output_dir"] = outputDir.generic_string();

    if (progress.finished && !progress.failed) {
        result["artifacts"] = artifactsJson(outputDir);
        std::string message;
        if (r.shadersOnly) {
            message = "Shaders generated in " + outputDir.generic_string() + ".";
        } else if (r.mode == ExportMode::Desktop) {
            const fs::path exe = outputDir / r.executableName;
            result["executable"] = exe.generic_string();
            message = "Built the Desktop player " + exe.generic_string() + ". Try it with run_build.";
            std::lock_guard<std::mutex> lock(manager().mutex);
            manager().lastExecutable = exe;
        } else if (r.mode == ExportMode::Web) {
            result["index_html"] = (outputDir / "index.html").generic_string();
            message = "Built the Web player in " + outputDir.generic_string() + " (serve it over HTTP; file:// does not load WebAssembly).";
        } else {
            message = "Exported the C++ source project to " + outputDir.generic_string() +
                      ". Build it with: cmake -S <dir> -B <dir>/build -DCMAKE_BUILD_TYPE=Release && cmake --build <dir>/build "
                      "(see its README / jasperengine.py for mobile and web).";
            result["how_to_build"] = "cmake -S \"" + outputDir.generic_string() + "\" -B \"" + (outputDir / "build").generic_string() +
                                     "\" -DCMAKE_BUILD_TYPE=Release && cmake --build \"" + (outputDir / "build").generic_string() + "\"";
        }
        if (r.warningCount) message += " " + std::to_string(r.warningCount) + " compiler warning(s).";
        r.job->succeed(message, result);
    } else if (r.job->cancelRequested()) {
        r.job->markCancelled("Build cancelled.");
    } else {
        std::string message = progress.errorMessage.empty() ? "Build failed." : progress.errorMessage;
        if (!message.empty() && message.back() != '.') message += ".";
        if (r.errorCount) {
            message += " " + std::to_string(r.errorCount) + " compiler error(s), first: ";
            for (const Diagnostic& d : r.diagnostics) {
                if (d.severity != "error") continue;
                message += (d.file.empty() ? std::string() : d.file + ":" + std::to_string(d.line) + ": ") + d.message;
                break;
            }
        }
        result["step"] = progress.currentStep;
        r.job->fail(message, result);
    }
    std::lock_guard<std::mutex> lock(manager().mutex);
    manager().lastBuild = r.job->toJson(0);
}

void watchRun(std::shared_ptr<BuildRun> run) {
    BuildRun& r = *run;
    auto drainLogs = [&r]() {
        while (true) {
            const std::vector<LogEntry> entries = Out::getLogsSince(r.logCursor, 1000);
            for (const LogEntry& entry : entries) {
                r.logCursor = entry.id;
                consumeLog(r, entry);
            }
            if (entries.size() < 1000) break;
        }
        if (r.logFile) r.logFile.flush();   // so the log file can be followed while it builds
    };

    ExportProgress progress;
    while (true) {
        drainLogs();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            progress = r.exporter->getProgress();
        }
        std::string step = progress.currentStep;
        if (!progress.detailLine.empty() && progress.detailLine.size() < 160) step += " " + stripAnsi(progress.detailLine);
        r.job->setProgress(progress.overallProgress, step);
        if (progress.finished || progress.failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    // the last lines (Out::info "Build completed", the error) are logged right after the flags
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    drainLogs();

    if (r.worker.joinable()) r.worker.join();
    {
        // joins the export thread, which has returned
        std::lock_guard<std::mutex> lock(r.mutex);
        r.exporter.reset();
    }
    finishRun(run, progress);
    if (r.logFile) r.logFile.close();
    std::lock_guard<std::mutex> lock(manager().mutex);
    if (manager().active == run) manager().active.reset();
}

// Starts the watcher; the export (or the shader worker) must have started
void startRun(const std::shared_ptr<BuildRun>& run) {
    run->job->setCancelHandler([weak = std::weak_ptr<BuildRun>(run)]() {
        if (auto r = weak.lock()) {
            std::lock_guard<std::mutex> lock(r->mutex);
            if (r->exporter) r->exporter->cancelExport();
        }
    });
    std::thread(watchRun, run).detach();
}

bool openRunLog(BuildRun& run, Project* project) {
    std::error_code ec;
    const fs::path dir = project->getProjectInternalPath() / "logs";
    fs::create_directories(dir, ec);
    run.logPath = dir / ("build-" + run.job->id() + ".log");
    run.logFile.open(run.logPath, std::ios::trunc);
    return static_cast<bool>(run.logFile);
}

ToolResult buildResult(const McpJob& job) {
    Json data = job.toJson(job.status() == JobStatus::Succeeded ? 5 : 30);
    std::string text = job.title() + ": " + jobStatusName(job.status()) + " - " + job.message();
    if (job.status() == JobStatus::Failed) {
        const Json result = job.result();
        if (result.contains("errors") && !result["errors"].empty()) {
            text += "\nErrors:";
            size_t shown = 0;
            for (const Json& e : result["errors"]) {
                if (++shown > 10) break;
                text += "\n  " + (e["file"].get<std::string>().empty() ? std::string()
                                    : e["file"].get<std::string>() + ":" + std::to_string(e["line"].get<int>()) + ": ") +
                        e["message"].get<std::string>();
            }
        } else {
            text += "\nLast log lines are in log_tail; the whole log in result.log_file.";
        }
        return ToolResult::error(text, data);
    }
    if (job.status() == JobStatus::Cancelled) return ToolResult::error(text, data);
    return ToolResult::success(text, data);
}

bool busy(const CallPtr& call) {
    std::lock_guard<std::mutex> lock(manager().mutex);
    if (manager().active) {
        call->fail("A build is already running (job " + manager().active->job->id() + "). Wait for it with get_job or stop it with cancel_job.",
                   manager().active->job->toJson(5));
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ build_project

void buildProjectTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();

    if (ctx.isPlaying()) {
        call->fail("Stop Play mode first: a build saves every scene.");
        return;
    }
    if (busy(call)) return;

    // ---- mode
    const std::string modeText = lowerText(args.value("mode", "desktop"));
    ExportMode mode;
    if (modeText == "source" || modeText == "source_code" || modeText == "sourcecode") mode = ExportMode::SourceCode;
    else if (modeText == "desktop" || modeText == "player") mode = ExportMode::Desktop;
    else if (modeText == "web") mode = ExportMode::Web;
    else {
        call->fail("mode must be desktop (a runnable player for this computer), web (WebAssembly player, needs emsdk) or "
                   "source (the C++ source project, for any platform).");
        return;
    }

    // ---- scenes
    bool hasSavedScene = false;
    for (const SceneProject& scene : project->getScenes()) hasSavedScene = hasSavedScene || !scene.filepath.empty();
    if (!hasSavedScene) {
        call->fail("No saved scene is in the build: save a scene first (save_project saves untitled scenes).");
        return;
    }

    ExportConfig config;
    config.mode = mode;
    config.assetsDir = project->getAssetsPath();
    config.luaDir = project->getLuaPath();
    config.packNativeResources = project->shouldPackNativeResources();
    std::string error;

    uint32_t startScene = project->getStartSceneId();
    if (args.contains("start_scene") && !args["start_scene"].is_null()) {
        SceneProject* scene = McpRefs::resolveScene(project, args["start_scene"], error);
        if (!scene) {
            call->fail("start_scene: " + error);
            return;
        }
        if (scene->filepath.empty()) {
            call->fail("start_scene \"" + scene->name + "\" is untitled: save it first.");
            return;
        }
        startScene = scene->id;
    }
    const SceneProject* start = project->getScene(startScene);
    if (!start || start->filepath.empty()) {
        startScene = NULL_PROJECT_SCENE;
        for (const SceneProject& scene : project->getScenes()) {
            if (!scene.filepath.empty()) {
                startScene = scene.id;
                break;
            }
        }
    }
    config.startSceneId = startScene;
    bool projectDirty = project->getStartSceneId() != startScene;
    project->setStartSceneId(startScene);

    // ---- output directory
    fs::path outputDir;
    if (args.contains("output_dir") && args["output_dir"].is_string() && !args["output_dir"].get<std::string>().empty()) {
        outputDir = fs::path(args["output_dir"].get<std::string>());
        if (!outputDir.is_absolute()) {
            call->fail("output_dir must be an absolute directory outside the project.");
            return;
        }
    } else {
        outputDir = defaultOutputDir(project, mode);
    }
    outputDir = outputDir.lexically_normal();
    if (outputDir.filename().empty()) outputDir = outputDir.parent_path();
    if ((isInside(outputDir, project->getProjectPath()) || isInside(outputDir, project->getAssetsPath())) &&
        !isInside(outputDir, project->getProjectInternalPath())) {
        call->fail("output_dir " + outputDir.generic_string() + " is inside the project (its assets and scenes are scanned "
                   "from there, so the exported copies would show up as project files). Use a directory outside the "
                   "project, e.g. " + (project->getProjectPath().parent_path() / (appName(project) + "-build")).generic_string() + ".");
        return;
    }
    const bool overwrite = args.value("overwrite", false);
    std::error_code ec;
    if (mode == ExportMode::SourceCode) {
        if (fs::exists(outputDir, ec) && !fs::is_empty(outputDir, ec) && !overwrite) {
            call->fail("output_dir " + outputDir.generic_string() + " is not empty: pass overwrite: true (files with the same "
                       "name are replaced; stale ones stay) or choose an empty directory.");
            return;
        }
        config.targetDir = outputDir;
        config.overwriteTarget = overwrite;
    } else {
        config.destinationDir = outputDir;
    }

    // ---- graphics APIs
    if (mode == ExportMode::SourceCode) {
        if (args.contains("backends") && !args["backends"].is_null()) {
            if (!parseBackends(args["backends"], config.selectedBackends, error)) {
                call->fail("backends: " + error + ".");
                return;
            }
            auto& settings = project->getSourceCodeExportSettings();
            projectDirty = projectDirty || !settings.graphicBackendsConfigured || settings.graphicBackends != config.selectedBackends;
            settings.graphicBackends = config.selectedBackends;
            settings.graphicBackendsConfigured = true;
        } else if (args.contains("platform") && args["platform"].is_string()) {
            if (!platformBackends(lowerText(args["platform"].get<std::string>()), config.selectedBackends)) {
                call->fail("platform must be windows, macos, linux, android, ios or web (or pass backends).");
                return;
            }
        } else if (project->getSourceCodeExportSettings().graphicBackendsConfigured &&
                   !project->getSourceCodeExportSettings().graphicBackends.empty()) {
            config.selectedBackends = project->getSourceCodeExportSettings().graphicBackends;
        } else {
            platformBackends(hostPlatformName(), config.selectedBackends);
        }
    } else {
        ShaderBackend backend = ShaderBackend::GLES3;
        if (mode == ExportMode::Desktop) {
            const std::vector<ShaderBackend> host = desktopBackends();
            const auto& settings = project->getDesktopExportSettings();
            backend = settings.graphicBackendConfigured &&
                      std::find(host.begin(), host.end(), settings.graphicBackend) != host.end()
                ? settings.graphicBackend : host.front();
            if (args.contains("graphics_api") && args["graphics_api"].is_string()) {
                ShaderBackend wanted;
                if (!ShaderPool::parseShaderBackend(args["graphics_api"].get<std::string>(), wanted) ||
                    std::find(host.begin(), host.end(), wanted) == host.end()) {
                    std::string valid;
                    for (ShaderBackend b : host) valid += " " + backendToken(b);
                    call->fail("graphics_api for a Desktop player on " + std::string(hostPlatformName()) + " is one of:" + valid + ".");
                    return;
                }
                backend = wanted;
                auto& desktop = project->getDesktopExportSettings();
                projectDirty = projectDirty || !desktop.graphicBackendConfigured || desktop.graphicBackend != backend;
                desktop.graphicBackend = backend;
                desktop.graphicBackendConfigured = true;
            }
        } else if (args.contains("graphics_api") && args["graphics_api"].is_string() &&
                   lowerText(args["graphics_api"].get<std::string>()) != "opengles") {
            call->fail("Web players use opengles (WebGL 2).");
            return;
        }
        config.selectedBackends.insert(backend);
        config.graphicBackend = Exporter::getCMakeGraphicBackend(backend);
    }

    // ---- toolchain
    const fs::path projectFile = project->getProjectPath() / "project.yaml";
    if (mode != ExportMode::SourceCode) {
        const std::string buildType = args.value("build_type", "Release");
        static const std::set<std::string> buildTypes = {"Release", "Debug", "RelWithDebInfo", "MinSizeRel"};
        if (!buildTypes.count(buildType)) {
            call->fail("build_type must be Release, Debug, RelWithDebInfo or MinSizeRel.");
            return;
        }
        config.buildType = buildType;
    }
    if (mode == ExportMode::Desktop) {
        const std::string missing = Generator::checkBuildTools();
        if (!missing.empty()) {
            call->fail("Missing build tools for a Desktop player:\n" + missing +
                       "Install them, or export mode source and build it elsewhere.");
            return;
        }
        const LocalBuildSettings build = AppSettings::getBuildSettings(projectFile);
        config.cmakeCCompiler = build.cCompiler;
        config.cmakeCxxCompiler = build.cxxCompiler;
        config.cmakeGenerator = build.generator;
        config.buildJobs = build.buildJobs;
        if (args.contains("build_jobs") && args["build_jobs"].is_number_integer()) {
            const long long jobs = args["build_jobs"].get<long long>();
            if (jobs < 0 || jobs > 1024) {
                call->fail("build_jobs must be 0 (automatic) to 1024.");
                return;
            }
            config.buildJobs = static_cast<unsigned int>(jobs);
        }
    } else if (mode == ExportMode::Web) {
        config.emsdkPath = AppSettings::getEmsdkPath();
        const EmsdkInfo emsdk = Exporter::detectEmsdk(config.emsdkPath);
        if (!emsdk.found) {
            call->fail("Emscripten SDK not found: set preferences.emsdk_path (set_project_settings) to the emsdk root, set "
                       "EMSDK, or put emcmake on PATH. Or export mode source.");
            return;
        }
    }

    // ---- shaders (persisted like the Build Settings window does)
    ShaderOverrides& overrides = project->getShaderOverrides();
    if (args.contains("shaders") && args["shaders"].is_object()) {
        const Json& shaders = args["shaders"];
        std::set<ShaderKey> additions = overrides.additions;
        std::set<ShaderKey> exclusions = overrides.exclusions;
        for (const char* field : {"add", "exclude", "reset"}) {
            if (!shaders.contains(field)) continue;
            if (std::string(field) == "reset") {
                if (shaders["reset"].is_boolean() && shaders["reset"].get<bool>()) {
                    additions.clear();
                    exclusions.clear();
                }
                continue;
            }
            if (!shaders[field].is_array()) {
                call->fail(std::string("shaders.") + field + " must be a list of shader keys (from get_build_info shaders).");
                return;
            }
            for (const Json& item : shaders[field]) {
                ShaderKey key;
                if (!parseShaderKey(item, key)) {
                    call->fail("Bad shader key " + item.dump() + ": use the key strings get_build_info lists (\"0x...\").");
                    return;
                }
                key = ShaderPool::normalizeKey(key);
                if (std::string(field) == "add") {
                    exclusions.erase(key);
                    additions.insert(key);
                } else {
                    additions.erase(key);
                    exclusions.insert(key);
                }
            }
        }
        projectDirty = projectDirty || additions != overrides.additions || exclusions != overrides.exclusions;
        overrides.additions = additions;
        overrides.exclusions = exclusions;
    }
    config.shaderAdditions = overrides.additions;
    config.shaderExclusions = overrides.exclusions;

    if (projectDirty && !project->saveProjectFile()) {
        call->fail("Could not write the project file (see the console).");
        return;
    }
    AppSettings::setExportTargetDir(projectFile, settingsModeKey(mode), outputDir);

    // ---- start
    const char* modeLabel = mode == ExportMode::SourceCode ? "Export source" : mode == ExportMode::Desktop ? "Build Desktop player" : "Build Web player";
    auto run = std::make_shared<BuildRun>();
    run->job = ctx.jobs().create("build", std::string(modeLabel) + (mode == ExportMode::SourceCode ? "" : " (" + config.buildType + ")"));
    run->mode = mode;
    run->config = config;
    run->projectPath = project->getProjectPath();
    run->executableName = executableName(project);
    run->exporter = std::make_unique<Exporter>();
    run->logCursor = Out::lastLogId();
    openRunLog(*run, project);
    {
        std::lock_guard<std::mutex> lock(manager().mutex);
        manager().active = run;
    }
    Out::info("MCP: %s to %s", modeLabel, outputDir.string().c_str());
    run->exporter->startExport(project, config);
    startRun(run);

    replyWhenJobDone(call, run->job, args.value("wait", true), args.value("timeout_seconds", 120.0), buildResult);
}

// ------------------------------------------------------------------ generate_shaders

void generateShadersTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    if (busy(call)) return;

    const std::string target = args.value("output_dir", "");
    if (target.empty() || !fs::path(target).is_absolute()) {
        call->fail("output_dir is required: an absolute directory for the generated shaders.");
        return;
    }
    ExportConfig config;
    config.targetDir = fs::path(target).lexically_normal();
    std::string error;
    if (!parseBackends(args.contains("backends") ? args["backends"] : Json("all"), config.selectedBackends, error)) {
        call->fail("backends: " + error + ".");
        return;
    }
    const std::string format = lowerText(args.value("format", "header"));
    if (format == "header") config.shaderOutputFormat = ShaderOutputFormat::Header;
    else if (format == "binary") config.shaderOutputFormat = ShaderOutputFormat::Binary;
    else if (format == "json") config.shaderOutputFormat = ShaderOutputFormat::Json;
    else {
        call->fail("format must be header, binary or json.");
        return;
    }

    std::set<ShaderKey> keys;
    if (args.contains("shaders") && args["shaders"].is_array() && !args["shaders"].empty()) {
        for (const Json& item : args["shaders"]) {
            ShaderKey key;
            if (!parseShaderKey(item, key)) {
                call->fail("Bad shader key " + item.dump() + ": use the key strings get_build_info lists (\"0x...\").");
                return;
            }
            keys.insert(ShaderPool::normalizeKey(key));
        }
    } else {
        keys = sceneShaderKeys(project);
        const ShaderOverrides& overrides = project->getShaderOverrides();
        for (ShaderKey key : overrides.additions) keys.insert(ShaderPool::normalizeKey(key));
        for (ShaderKey key : overrides.exclusions) keys.erase(ShaderPool::normalizeKey(key));
    }
    if (keys.empty()) {
        call->fail("No shaders to generate: the scenes need none yet (add GameObjects that render), or pass shaders.");
        return;
    }
    config.selectedShaderKeys = keys;

    auto run = std::make_shared<BuildRun>();
    run->job = ctx.jobs().create("shaders", "Generate " + std::to_string(keys.size()) + " shader(s)");
    run->shadersOnly = true;
    run->config = config;
    run->projectPath = project->getProjectPath();
    run->exporter = std::make_unique<Exporter>();
    run->logCursor = Out::lastLogId();
    openRunLog(*run, project);
    {
        std::lock_guard<std::mutex> lock(manager().mutex);
        manager().active = run;
    }
    // Shader generation runs the exporter on the calling thread: a worker, not the main thread.
    // project is not used (no scene saving), so it never waits for the main thread.
    Exporter* exporter = run->exporter.get();
    run->worker = std::thread([exporter, config]() { exporter->generateShaders(config); });
    startRun(run);
    replyWhenJobDone(call, run->job, args.value("wait", true), args.value("timeout_seconds", 120.0), buildResult);
}

// ------------------------------------------------------------------ get_build_info

void getBuildInfoTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const fs::path projectFile = project->getProjectPath() / "project.yaml";

    Json data;
    data["host_platform"] = hostPlatformName();

    const std::string missing = Generator::checkBuildTools();
    const LocalBuildSettings kit = AppSettings::getBuildSettings(projectFile);
    Json desktopApis = Json::array();
    for (ShaderBackend b : desktopBackends()) desktopApis.push_back(backendToken(b));
    const auto& desktopSettings = project->getDesktopExportSettings();
    data["desktop"] = {
        {"available", missing.empty()},
        {"missing_tools", missing},
        {"graphics_apis", desktopApis},
        {"graphics_api", backendToken(desktopSettings.graphicBackendConfigured ? desktopSettings.graphicBackend : desktopBackends().front())},
        {"executable_name", executableName(project)},
        {"kit", {{"c_compiler", kit.cCompiler}, {"cxx_compiler", kit.cxxCompiler}, {"generator", kit.generator},
                 {"build_jobs", kit.buildJobs}, {"automatic_jobs", Generator::getAutomaticParallelBuildJobs()}}},
        {"output_dir", defaultOutputDir(project, ExportMode::Desktop).generic_string()}
    };
    const EmsdkInfo emsdk = Exporter::detectEmsdk(AppSettings::getEmsdkPath());
    data["web"] = {
        {"available", emsdk.found},
        {"emsdk", emsdk.found ? emsdk.description : std::string("not found (set preferences.emsdk_path)")},
        {"output_dir", defaultOutputDir(project, ExportMode::Web).generic_string()}
    };
    const auto& sourceSettings = project->getSourceCodeExportSettings();
    std::set<ShaderBackend> sourceBackends;
    if (sourceSettings.graphicBackendsConfigured && !sourceSettings.graphicBackends.empty()) sourceBackends = sourceSettings.graphicBackends;
    else platformBackends(hostPlatformName(), sourceBackends);
    data["source"] = {
        {"available", true},
        {"backends", backendList(sourceBackends)},
        {"platforms", {"windows", "macos", "linux", "android", "ios", "web"}},
        {"output_dir", defaultOutputDir(project, ExportMode::SourceCode).generic_string()}
    };
    data["all_backends"] = allBackendTokens();

    Json scenes = Json::array();
    const uint32_t startId = project->getStartSceneId();
    for (const SceneProject& scene : project->getScenes()) {
        if (scene.filepath.empty()) continue;
        scenes.push_back({{"id", scene.id}, {"name", scene.name}, {"start", scene.id == startId}});
    }
    data["scenes"] = scenes;

    const std::set<ShaderKey> fromScenes = sceneShaderKeys(project);
    const ShaderOverrides& overrides = project->getShaderOverrides();
    std::set<ShaderKey> all = fromScenes;
    for (ShaderKey key : overrides.additions) all.insert(ShaderPool::normalizeKey(key));
    for (ShaderKey key : overrides.exclusions) all.insert(ShaderPool::normalizeKey(key));
    size_t included = 0;
    Json shaders = Json::array();
    for (ShaderKey key : all) {
        const bool excluded = overrides.exclusions.count(key) > 0;
        if (!excluded) included++;
        if (args.value("include_shaders", false)) {
            Json item = {{"key", shaderKeyText(key)}, {"name", shaderName(key)}, {"from_scenes", fromScenes.count(key) > 0}};
            if (excluded) item["excluded"] = true;
            shaders.push_back(item);
        }
    }
    data["shader_count"] = included;
    if (args.value("include_shaders", false)) data["shaders"] = shaders;

    {
        std::lock_guard<std::mutex> lock(manager().mutex);
        data["running_job"] = manager().active ? Json(manager().active->job->id()) : Json(nullptr);
        data["last_build"] = manager().lastBuild;
        data["last_executable"] = manager().lastExecutable.empty() ? Json(nullptr) : Json(manager().lastExecutable.generic_string());
    }

    std::string text = std::string("Host ") + hostPlatformName() + ". Desktop player: " +
                       (missing.empty() ? "available" : "missing tools") + "; Web: " + (emsdk.found ? "available" : "no emsdk") +
                       "; source export: available. " + std::to_string(scenes.size()) + " scene(s) in the build, " +
                       std::to_string(included) + " shader(s).";
    call->ok(text, data);
}

// ------------------------------------------------------------------ run_build

// Lines of a log that mean the program crashed or can't work (tools/smoke-test.sh fatal_lines)
bool isFatalLine(const std::string& line) {
    static const std::regex fatal(R"(\(PANIC\)|[Mm]issing (precompiled )?shader|[Aa]ssertion|Aborted|Segmentation fault|terminate called|core dumped|AddressSanitizer|stack smashing)");
    return std::regex_search(line, fatal);
}

// Error lines of the player's log: Log::error ("(ERROR): ..."), Lua errors (a failed init(), a
// script that did not load)
bool isErrorLine(const std::string& line) {
    static const std::regex error(R"(\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|Lua \w+\(\) failed|Failed to (load|execute) Lua|Script crash)");
    if (line.find("Failed to initialize audio output") != std::string::npos) return false;
    return std::regex_search(line, error);
}

bool isWarningLine(const std::string& line) {
    return line.rfind("(WARN)", 0) == 0;
}

// run_build's input script → timed actions (the send_input vocabulary; frames are 1/60 s)
bool parsePlayerInput(const Json& events, const std::string& defaultSpace, double gap, std::vector<PlayerAction>& out,
                      double& end, std::string& error) {
    double t = 0.0;
    auto lower = [](std::string text) { return lowerText(std::move(text)); };
    auto seconds = [](const Json& e, const char* msKey, const char* framesKey, const char* secondsKey, double fallback) {
        if (e.contains(secondsKey) && e[secondsKey].is_number()) return std::max(0.0, e[secondsKey].get<double>());
        if (e.contains(msKey) && e[msKey].is_number()) return std::max(0.0, e[msKey].get<double>() / 1000.0);
        if (e.contains(framesKey) && e[framesKey].is_number()) return std::max(0.0, e[framesKey].get<double>() / 60.0);
        return fallback;
    };
    auto readPoint = [](const Json& e, const char* xKey, const char* yKey, const char* arrayKey, double& x, double& y) {
        if (e.contains(arrayKey) && e[arrayKey].is_array() && e[arrayKey].size() >= 2 && e[arrayKey][0].is_number() &&
            e[arrayKey][1].is_number()) {
            x = e[arrayKey][0].get<double>();
            y = e[arrayKey][1].get<double>();
            return true;
        }
        if (e.contains(xKey) && e.contains(yKey) && e[xKey].is_number() && e[yKey].is_number()) {
            x = e[xKey].get<double>();
            y = e[yKey].get<double>();
            return true;
        }
        return false;
    };
    auto add = [&](PlayerAction::Kind kind, double at) -> PlayerAction& {
        PlayerAction action;
        action.kind = kind;
        action.at = at;
        out.push_back(action);
        return out.back();
    };
    static const std::map<std::string, std::string> modKeys = {
        {"shift", "shift"}, {"ctrl", "ctrl"}, {"control", "ctrl"}, {"alt", "alt"}, {"super", "super"}, {"cmd", "super"}, {"meta", "super"}};

    for (size_t i = 0; i < events.size(); i++) {
        const Json& e = events[i];
        const std::string where = "input[" + std::to_string(i) + "]: ";
        if (!e.is_object()) {
            error = where + "each event must be an object with a type";
            return false;
        }
        const std::string type = lower(e.value("type", ""));
        const std::string space = lower(e.value("space", defaultSpace));
        if (space != "window" && space != "normalized") {
            error = where + "space must be window (player window pixels from the top-left) or normalized (0..1)";
            return false;
        }
        const double hold = seconds(e, "hold_ms", "hold_frames", "hold_seconds", 0.1);
        std::vector<unsigned long> mods;
        if (e.contains("mods") && e["mods"].is_array()) {
            for (const Json& m : e["mods"]) {
                auto it = modKeys.find(m.is_string() ? lower(m.get<std::string>()) : std::string());
                if (it == modKeys.end()) {
                    error = where + "mods are shift, ctrl, alt, super";
                    return false;
                }
                mods.push_back(playerKeysym(it->second));
            }
        }
        if (type == "wait") {
            const double w = seconds(e, "ms", "frames", "seconds", -1.0);
            if (w < 0.0) {
                error = where + "wait needs ms, frames (1/60 s) or seconds";
                return false;
            }
            t += w;
            continue;
        }
        if (type == "screenshot") {
            add(PlayerAction::Screenshot, t);
            t += gap;
            continue;
        }
        if (type == "release_all") {
            add(PlayerAction::ReleaseAll, t);
            t += gap;
            continue;
        }
        if (type == "key") {
            const Json key = e.contains("key") ? e["key"] : Json();
            const unsigned long sym = key.is_string() ? playerKeysym(lower(key.get<std::string>())) : 0;
            if (!sym) {
                error = where + "unknown key " + key.dump() + " (names like space, enter, escape, a-z, 0-9, left, right, "
                        "up, down, shift, ctrl, alt, tab, backspace, f1-f12, kp_0-kp_9)";
                return false;
            }
            const std::string action = lower(e.value("action", "press"));
            if (action != "press" && action != "tap" && action != "down" && action != "up") {
                error = where + "key action must be press, down or up";
                return false;
            }
            if (action != "up") {
                for (unsigned long m : mods) add(PlayerAction::KeyDown, t).keysym = m;
                add(PlayerAction::KeyDown, t).keysym = sym;
            }
            if (action == "press" || action == "tap") t += hold;
            if (action != "down") {
                add(PlayerAction::KeyUp, t).keysym = sym;
                for (unsigned long m : mods) add(PlayerAction::KeyUp, t).keysym = m;
            }
            t += gap;
            continue;
        }
        if (type == "text") {
            const std::string text = e.value("text", "");
            if (text.empty()) {
                error = where + "text event needs text";
                return false;
            }
            const unsigned long shiftSym = playerKeysym("shift");
            for (size_t k = 0; k < text.size();) {
                const unsigned char c = static_cast<unsigned char>(text[k]);
                uint32_t cp = c;
                size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                if (len > 1) cp = c & (0xFF >> (len + 1));
                for (size_t j = 1; j < len && k + j < text.size(); j++) cp = (cp << 6) | (static_cast<unsigned char>(text[k + j]) & 0x3F);
                k += len;
                bool shift = false;
                const unsigned long sym = playerCharKeysym(cp, shift);
                if (!sym) {
                    error = where + "can't type character U+" + std::to_string(cp);
                    return false;
                }
                if (shift) add(PlayerAction::KeyDown, t).keysym = shiftSym;
                add(PlayerAction::KeyDown, t).keysym = sym;
                t += 0.03;
                add(PlayerAction::KeyUp, t).keysym = sym;
                if (shift) add(PlayerAction::KeyUp, t).keysym = shiftSym;
                t += 0.03;
            }
            t += gap;
            continue;
        }
        if (type == "mouse") {
            const std::string action = lower(e.value("action", "click"));
            const std::string buttonName = e.contains("button") && e["button"].is_string() ? lower(e["button"].get<std::string>()) : "left";
            const int button = buttonName == "left" ? 1 : buttonName == "middle" ? 2 : buttonName == "right" ? 3 : 0;
            if (!button) {
                error = where + "button must be left, right or middle";
                return false;
            }
            double x = 0, y = 0;
            const bool hasPoint = readPoint(e, "x", "y", "position", x, y);
            if (!hasPoint && action != "scroll" && action != "up" && action != "down") {
                error = where + "mouse " + action + " needs x and y (or position [x,y])";
                return false;
            }
            const bool normalized = space == "normalized";
            auto moveTo = [&](double px, double py, double at) {
                PlayerAction& m = add(PlayerAction::Move, at);
                m.x = px;
                m.y = py;
                m.normalized = normalized;
            };
            if (hasPoint) moveTo(x, y, t);
            if (action == "move") {
            } else if (action == "down") {
                add(PlayerAction::ButtonDown, t).button = button;
            } else if (action == "up") {
                add(PlayerAction::ButtonUp, t).button = button;
            } else if (action == "click" || action == "double_click") {
                const int clicks = action == "double_click" ? 2 : 1;
                for (int c = 0; c < clicks; c++) {
                    add(PlayerAction::ButtonDown, t).button = button;
                    t += clicks == 2 ? std::min(hold, 0.08) : hold;
                    add(PlayerAction::ButtonUp, t).button = button;
                    if (c + 1 < clicks) t += 0.08;
                }
            } else if (action == "drag") {
                double tx = 0, ty = 0;
                if (!readPoint(e, "to_x", "to_y", "to", tx, ty)) {
                    error = where + "mouse drag needs to: [x,y]";
                    return false;
                }
                const int steps = std::clamp(e.value("steps", 10), 1, 1000);
                add(PlayerAction::ButtonDown, t).button = button;
                for (int k = 1; k <= steps; k++) {
                    t += 1.0 / 60.0;
                    moveTo(x + (tx - x) * k / steps, y + (ty - y) * k / steps, t);
                }
                t += 1.0 / 60.0;
                add(PlayerAction::ButtonUp, t).button = button;
            } else if (action == "scroll") {
                const double dy = e.value("dy", e.value("amount", 1.0));
                const double dx = e.value("dx", 0.0);
                auto clicks = [&](double amount, int positive, int negative) {
                    for (int k = 0; k < static_cast<int>(std::round(std::abs(amount))); k++) {
                        const int b = amount > 0 ? positive : negative;
                        add(PlayerAction::ButtonDown, t).button = b;
                        add(PlayerAction::ButtonUp, t).button = b;
                        t += 0.02;
                    }
                };
                clicks(dy, 4, 5);
                clicks(dx, 7, 6);
            } else {
                error = where + "mouse action must be click, double_click, down, up, move, drag or scroll";
                return false;
            }
            t += gap;
            continue;
        }
        if (type == "touch" || type == "gamepad") {
            error = where + type + " input can't be sent to a built player (keys, text and mouse only)";
            return false;
        }
        error = where + "type must be key, text, mouse, wait, screenshot or release_all";
        return false;
    }
    end = t;
    return true;
}

#ifndef _WIN32

struct PlayerShot {
    double at = 0.0;                 // seconds since launch
    bool done = false;
    std::string label;               // "screenshots_at", "input"
    ScreenCapture capture;
};

struct PlayerRun {
    pid_t pid = -1;
    int fd = -1;
    std::string output;              // everything read (capped, tail kept)
    size_t droppedBytes = 0;
    bool exited = false;
    int status = 0;
    bool terminating = false;
    bool killedByUs = false;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point terminateAt;
    std::chrono::steady_clock::time_point exitedAt;
    double seconds = 5.0;
    int maxWidth = 1280;
    std::vector<unsigned long> windowsBefore;
    unsigned long window = 0;        // the player's window once found
    std::vector<PlayerShot> shots;   // timed screenshots (sorted)
    fs::path exe;
    fs::path logPath;
    // input script
    std::vector<PlayerAction> actions;
    size_t nextAction = 0;
    double inputDelay = 1.0;
    double inputStartedAt = -1.0;    // seconds since launch
    PlayerInputSender* sender = nullptr;
    std::string inputMethod;
    std::string inputError;
};

constexpr size_t MAX_PLAYER_OUTPUT = 4 * 1024 * 1024;

void readPlayerOutput(PlayerRun& run) {
    if (run.fd < 0) return;
    char buffer[16384];
    while (true) {
        const ssize_t n = ::read(run.fd, buffer, sizeof(buffer));
        if (n > 0) {
            run.output.append(buffer, static_cast<size_t>(n));
            if (run.output.size() > MAX_PLAYER_OUTPUT) {
                const size_t drop = run.output.size() - MAX_PLAYER_OUTPUT / 2;
                run.output.erase(0, drop);
                run.droppedBytes += drop;
            }
            continue;
        }
        if (n == 0) {   // EOF: every writer closed
            ::close(run.fd);
            run.fd = -1;
        }
        return;   // EAGAIN or error
    }
}

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void closeRunInput(PlayerRun& run) {
    if (run.sender) {
        closePlayerInput(run.sender, run.window);
        run.sender = nullptr;
    }
}

// Due input actions: one time slot per editor frame, so a press always spans a frame
void runPlayerInput(PlayerRun& run, double elapsed) {
    if (run.actions.empty() || run.nextAction >= run.actions.size() || !run.inputError.empty() || run.exited) return;
    if (run.inputStartedAt < 0.0) {
        if (elapsed < run.inputDelay) return;
        if (!run.window) run.window = findPlayerWindow(run.pid, run.windowsBefore);
        if (!run.window) {
            if (elapsed > run.inputDelay + 10.0) run.inputError = "the player opened no window to send the input to";
            return;
        }
        std::string error;
        run.sender = openPlayerInput(run.inputMethod, error);
        if (!run.sender) {
            run.inputError = error;
            return;
        }
        run.inputStartedAt = elapsed;
    }
    const double t = elapsed - run.inputStartedAt;
    if (run.actions[run.nextAction].at > t) return;
    const double slot = run.actions[run.nextAction].at;
    while (run.nextAction < run.actions.size() && run.actions[run.nextAction].at <= slot) {
        const PlayerAction& action = run.actions[run.nextAction++];
        if (action.kind == PlayerAction::Screenshot) {
            PlayerShot shot;
            shot.at = elapsed;
            shot.done = true;
            shot.label = "input";
            shot.capture = captureWindow(run.window, run.maxWidth);
            run.shots.push_back(std::move(shot));
            continue;
        }
        const std::string error = sendPlayerAction(run.sender, run.window, action);
        if (!error.empty()) {
            run.inputError = "input action " + std::to_string(run.nextAction) + ": " + error;
            return;
        }
    }
}

void finishPlayerRun(const CallPtr& call, const std::shared_ptr<PlayerRun>& run) {
    closeRunInput(*run);
    readPlayerOutput(*run);
    if (run->fd >= 0) {
        ::close(run->fd);
        run->fd = -1;
    }

    // split into lines
    std::vector<std::string> lines;
    {
        std::stringstream ss(run->output);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    Json fatal = Json::array();
    Json errors = Json::array();
    size_t errorCount = 0, warningCount = 0;
    for (const std::string& line : lines) {
        if (isFatalLine(line) && fatal.size() < 30) fatal.push_back(line);
        if (isErrorLine(line)) {
            errorCount++;
            if (errors.size() < 30) errors.push_back(line.size() > 2000 ? line.substr(0, 2000) + "..." : line);
        } else if (isWarningLine(line)) {
            warningCount++;
        }
    }

    if (!run->logPath.empty()) {
        std::ofstream out(run->logPath, std::ios::trunc | std::ios::binary);
        out << run->output;
    }

    const int maxLines = std::max(0, std::min(2000, call->args().value("log_lines", 80)));
    Json tail = Json::array();
    for (size_t i = lines.size() > static_cast<size_t>(maxLines) ? lines.size() - maxLines : 0; i < lines.size(); i++) {
        tail.push_back(lines[i].size() > 2000 ? lines[i].substr(0, 2000) + "..." : lines[i]);
    }

    const double ran = std::chrono::duration<double>((run->exited ? run->exitedAt : std::chrono::steady_clock::now()) - run->started).count();
    Json data = {
        {"executable", run->exe.generic_string()},
        {"pid", run->pid},
        {"ran_seconds", ran},
        {"log_lines", lines.size()},
        {"log", tail},
        {"errors", errors},
        {"error_count", errorCount},
        {"warning_count", warningCount},
        {"fatal_lines", fatal},
        {"log_file", run->logPath.generic_string()}
    };
    bool crashed = !fatal.empty();
    std::string how;
    if (run->killedByUs) {
        data["stopped_by_run_build"] = true;
        data["exit_code"] = nullptr;
        how = "ran for " + std::to_string(static_cast<int>(run->seconds)) + " s and was stopped";
    } else if (WIFEXITED(run->status)) {
        data["exit_code"] = WEXITSTATUS(run->status);
        data["exited_early"] = true;
        how = "exited by itself after " + std::to_string(static_cast<int>(ran + 0.5)) + " s with code " + std::to_string(WEXITSTATUS(run->status));
        if (WEXITSTATUS(run->status) != 0) crashed = true;
    } else if (WIFSIGNALED(run->status)) {
        const int sig = WTERMSIG(run->status);
        data["signal"] = sig;
        data["exited_early"] = true;
        crashed = true;
        how = "died from signal " + std::to_string(sig) + (sig == SIGSEGV ? " (segmentation fault)" : sig == SIGABRT ? " (abort)" : "");
    }
    data["crashed"] = crashed;

    ToolResult result;
    std::string text = run->exe.filename().string() + " " + how + ". " + std::to_string(lines.size()) + " log line(s)";
    if (errorCount) text += ", " + std::to_string(errorCount) + " ERROR(S): " + errors[0].get<std::string>();
    if (!fatal.empty()) text += ", fatal: " + fatal[0].get<std::string>();
    text += ".";

    if (!run->actions.empty()) {
        Json input = {
            {"actions_done", run->nextAction},
            {"actions_total", run->actions.size()},
            {"method", run->inputMethod.empty() ? Json(nullptr) : Json(run->inputMethod)}
        };
        if (run->inputStartedAt >= 0.0) input["started_at_seconds"] = std::round(run->inputStartedAt * 1000.0) / 1000.0;
        if (!run->inputError.empty()) input["error"] = run->inputError;
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (run->window && !run->exited && playerWindowRect(run->window, wx, wy, ww, wh)) {
            input["window"] = {{"width", ww}, {"height", wh}};
        }
        data["input"] = input;
        if (!run->inputError.empty()) text += " Input failed: " + run->inputError + ".";
        else if (run->nextAction < run->actions.size()) text += " Input: " + std::to_string(run->nextAction) + "/" +
                                                                std::to_string(run->actions.size()) + " action(s) sent before the run ended.";
        else text += " Input: all " + std::to_string(run->actions.size()) + " action(s) sent (" + run->inputMethod + ").";
    }

    std::stable_sort(run->shots.begin(), run->shots.end(), [](const PlayerShot& a, const PlayerShot& b) { return a.at < b.at; });
    Json shots = Json::array();
    for (const PlayerShot& shot : run->shots) {
        Json item = {{"at_seconds", std::round(shot.at * 100.0) / 100.0}, {"from", shot.label}};
        if (shot.capture.ok) {
            item["image"] = static_cast<int>(result.images.size());
            item["width"] = shot.capture.width;
            item["height"] = shot.capture.height;
            item["note"] = shot.capture.note;
            result.images.push_back({Base64::encode(shot.capture.png.data(), shot.capture.png.size()), "image/png"});
        } else {
            item["error"] = !shot.done ? "not taken: the player exited first" : shot.capture.note;
        }
        shots.push_back(item);
    }
    if (shots.size() == 1 && run->shots[0].label == "screenshot") {
        // the single screenshot of the plain call
        const PlayerShot& shot = run->shots[0];
        if (shot.capture.ok) {
            data["screenshot"] = {{"width", shot.capture.width}, {"height", shot.capture.height}, {"note", shot.capture.note}};
            text += " Screenshot: " + shot.capture.note + ".";
        } else {
            data["screenshot"] = nullptr;
            data["screenshot_error"] = shots[0]["error"];
            text += " No screenshot: " + shots[0]["error"].get<std::string>() + ".";
        }
    } else if (!shots.empty()) {
        data["screenshots"] = shots;
        text += " " + std::to_string(result.images.size()) + " screenshot(s), in time order (structured screenshots[]).";
    }
    if (!tail.empty()) {
        text += "\nLast log lines:";
        const size_t shown = std::min<size_t>(tail.size(), 15);
        for (size_t i = tail.size() - shown; i < tail.size(); i++) text += "\n  " + tail[i].get<std::string>();
    }
    result.text = text;
    result.structured = data;
    result.isError = false;   // a crash or a script error is a finding about the game, not a tool failure
    call->reply(std::move(result));
}

void runBuildTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();

    fs::path exe;
    const std::string pathText = args.value("path", "");
    if (!pathText.empty()) {
        exe = fs::path(pathText);
        if (!exe.is_absolute()) {
            call->fail("path must be absolute: the player executable or the build's output directory.");
            return;
        }
    } else {
        std::lock_guard<std::mutex> lock(manager().mutex);
        exe = manager().lastExecutable;
        if (exe.empty()) exe = defaultOutputDir(project, ExportMode::Desktop);
    }
    std::error_code ec;
    if (fs::is_directory(exe, ec)) {
        const fs::path dir = exe;
        exe = dir / executableName(project);
        if (!fs::is_regular_file(exe, ec)) {
            // the only executable file in it
            std::vector<fs::path> found;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec) && ::access(entry.path().c_str(), X_OK) == 0 &&
                    entry.path().extension() != ".so" && entry.path().extension() != ".sh") {
                    found.push_back(entry.path());
                }
            }
            if (found.size() != 1) {
                call->fail("No player executable in " + dir.generic_string() + " (expected " + executableName(project) +
                           "). Build one with build_project mode desktop, or pass path.");
                return;
            }
            exe = found.front();
        }
    }
    if (!fs::is_regular_file(exe, ec) || ::access(exe.c_str(), X_OK) != 0) {
        call->fail("No executable at " + exe.generic_string() + ". Build a Desktop player with build_project first, or pass path.");
        return;
    }

    auto run = std::make_shared<PlayerRun>();
    run->exe = exe;
    run->maxWidth = std::max(64, std::min(4096, args.value("max_width", 1280)));

    // input script
    double inputEnd = 0.0;
    if (args.contains("input") && !args["input"].is_null()) {
        const std::string why = playerInputUnsupported();
        if (!why.empty()) {
            call->fail("input is not supported here: " + why + ". Run without input (log + screenshots only).");
            return;
        }
        if (!args["input"].is_array() || args["input"].empty() || args["input"].size() > 500) {
            call->fail("input must be an array of 1-500 events, e.g. [{\"type\":\"key\",\"key\":\"space\"}, "
                       "{\"type\":\"wait\",\"ms\":500}, {\"type\":\"screenshot\"}].");
            return;
        }
        std::string error;
        const std::string space = lowerText(args.value("space", "window"));
        const double gap = std::clamp(args.value("gap_ms", 50.0), 0.0, 10000.0) / 1000.0;
        if (!parsePlayerInput(args["input"], space, gap, run->actions, inputEnd, error)) {
            call->fail(error + ".");
            return;
        }
        std::stable_sort(run->actions.begin(), run->actions.end(), [](const PlayerAction& a, const PlayerAction& b) { return a.at < b.at; });
        run->inputDelay = std::clamp(args.value("input_delay_seconds", 1.0), 0.0, 120.0);
    }

    // screenshots: screenshots_at (seconds since launch), else the single screenshot
    double lastShot = 0.0;
    if (args.contains("screenshots_at") && !args["screenshots_at"].is_null()) {
        if (!args["screenshots_at"].is_array() || args["screenshots_at"].size() > 20) {
            call->fail("screenshots_at must be a list of at most 20 times in seconds since launch, e.g. [2, 4.5].");
            return;
        }
        for (const Json& at : args["screenshots_at"]) {
            if (!at.is_number() || at.get<double>() < 0.0 || at.get<double>() > 300.0) {
                call->fail("screenshots_at entries are seconds since launch (0-300).");
                return;
            }
            PlayerShot shot;
            shot.at = std::max(0.1, at.get<double>());
            shot.label = "screenshots_at";
            run->shots.push_back(shot);
            lastShot = std::max(lastShot, shot.at);
        }
    }
    const bool hasInputShots = std::any_of(run->actions.begin(), run->actions.end(),
                                           [](const PlayerAction& a) { return a.kind == PlayerAction::Screenshot; });

    // how long: seconds, else long enough for the input and the screenshots
    double defaultSeconds = 5.0;
    if (!run->actions.empty()) defaultSeconds = std::max(defaultSeconds, run->inputDelay + inputEnd + 1.5);
    if (lastShot > 0.0) defaultSeconds = std::max(defaultSeconds, lastShot + 0.5);
    run->seconds = std::max(0.5, std::min(300.0, args.value("seconds", defaultSeconds)));

    const bool wantScreenshot = args.value("screenshot", run->shots.empty() && !hasInputShots);
    if (wantScreenshot && run->shots.empty()) {
        PlayerShot shot;
        const double after = args.value("screenshot_after_seconds", std::min(3.0, run->seconds * 0.8));
        shot.at = std::max(0.1, std::min(after, run->seconds));
        shot.label = "screenshot";
        run->shots.push_back(shot);
    }
    if (!run->shots.empty() || !run->actions.empty()) run->windowsBefore = listTopLevelWindows();
    {
        const fs::path logs = project->getProjectInternalPath() / "logs";
        fs::create_directories(logs, ec);
        static std::atomic<unsigned> counter{0};
        run->logPath = logs / ("run-" + std::to_string(++counter) + ".log");
    }

    // stdbuf: a player's stdout into a pipe is block-buffered, and lost when it is killed
    std::vector<std::string> argStrings;
    for (const char* stdbuf : {"/usr/bin/stdbuf", "/bin/stdbuf"}) {
        if (::access(stdbuf, X_OK) == 0) {
            argStrings = {stdbuf, "-oL", "-eL"};
            break;
        }
    }
    argStrings.push_back(exe.string());
    if (args.contains("args") && args["args"].is_array()) {
        for (const Json& a : args["args"]) {
            if (!a.is_string()) {
                call->fail("args must be strings.");
                return;
            }
            argStrings.push_back(a.get<std::string>());
        }
    }
    std::vector<char*> argv;
    for (std::string& s : argStrings) argv.push_back(s.data());
    argv.push_back(nullptr);
    const std::string workDir = exe.parent_path().string();

    int pipeFds[2];
    if (::pipe(pipeFds) != 0) {
        call->fail("Could not create a pipe for the player's output.");
        return;
    }
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        call->fail("Could not start the player (fork failed).");
        return;
    }
    if (pid == 0) {
        // child: async-signal-safe calls only
        ::setpgid(0, 0);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) ::dup2(devNull, 0);
        ::dup2(pipeFds[1], 1);
        ::dup2(pipeFds[1], 2);
        for (long fd = 3; fd < maxFd; fd++) ::close(static_cast<int>(fd));
        if (::chdir(workDir.c_str()) != 0) ::_exit(126);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(pipeFds[1]);
    ::fcntl(pipeFds[0], F_SETFL, ::fcntl(pipeFds[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    run->pid = pid;
    run->fd = pipeFds[0];
    run->started = std::chrono::steady_clock::now();
    Out::info("MCP run_build: started %s (pid %d)", exe.string().c_str(), static_cast<int>(pid));

    const int timeoutMs = static_cast<int>((run->seconds + 15.0) * 1000.0);
    call->waitUntil([run]() {
        readPlayerOutput(*run);
        if (!run->exited) {
            int status = 0;
            const pid_t done = ::waitpid(run->pid, &status, WNOHANG);
            if (done == run->pid) {
                run->exited = true;
                run->status = status;
                run->exitedAt = std::chrono::steady_clock::now();
                closeRunInput(*run);
            }
        }
        const double elapsed = secondsSince(run->started);
        if (!run->exited && !run->terminating) {
            runPlayerInput(*run, elapsed);
            for (PlayerShot& shot : run->shots) {
                if (shot.done || elapsed < shot.at) continue;
                shot.done = true;
                if (!run->window) run->window = findPlayerWindow(run->pid, run->windowsBefore);
                shot.capture = captureWindow(run->window, run->maxWidth);
                shot.at = elapsed;
                break;   // one per frame
            }
        }
        if (run->exited) {
            // the rest of the output: wait for EOF briefly (a child of the player may hold the pipe)
            return run->fd < 0 || secondsSince(run->exitedAt) > 1.0;
        }
        if (!run->terminating && elapsed >= run->seconds) {
            run->terminating = true;
            run->killedByUs = true;
            run->terminateAt = std::chrono::steady_clock::now();
            closeRunInput(*run);
            ::kill(-run->pid, SIGTERM);
            ::kill(run->pid, SIGTERM);
        } else if (run->terminating && secondsSince(run->terminateAt) > 3.0) {
            ::kill(-run->pid, SIGKILL);
            ::kill(run->pid, SIGKILL);
        }
        return false;
    }, timeoutMs, [call, run](bool timedOut) {
        if (!run->exited) {
            ::kill(-run->pid, SIGKILL);
            ::kill(run->pid, SIGKILL);
            int status = 0;
            ::waitpid(run->pid, &status, 0);
            run->exited = true;
            run->status = status;
            run->exitedAt = std::chrono::steady_clock::now();
            run->killedByUs = true;
        }
        (void)timedOut;
        finishPlayerRun(call, run);
    });
}

#else

void runBuildTool(const CallPtr& call) {
    call->fail("run_build is not implemented on Windows yet: start the player from its output directory.");
}

#endif

} // namespace

void registerBuildTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "get_build_info";
        tool.title = "Get Build Info";
        tool.description =
            "Build Settings state: toolchains found (cmake/compiler, emsdk; missing ones listed), graphics "
            "APIs and last output dir per mode, jobs, scenes in build, shader count (include_shaders lists "
            "keys for build_project shaders), running job, last result. Call before build_project.";
        tool.inputSchema = object({
            {"include_shaders", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getBuildInfoTool;
        registry.addTool(std::move(tool));
    }

    const Json jobTimeout = withDefault(number("", 0, 3600), 120);
    const Json stringItem = Json{{"type", "string"}};

    {
        ToolDef tool;
        tool.name = "build_project";
        tool.title = "Build Project";
        tool.description =
            "File > Build Settings > Build, as a job. desktop: player for this computer (the first build "
            "compiles the engine: many minutes); web: WebAssembly (needs emsdk); source: standalone C++/CMake "
            "project. Saves all scenes first. Past timeout_seconds returns {job_id} (poll get_job). Result: "
            "output_dir, artifacts, executable, errors [{file, line, message}], warnings, log_file. Don't edit "
            "scenes or play meanwhile.";
        tool.inputSchema = object({
            {"mode", withDefault(stringEnum("", {"desktop", "web", "source"}), "desktop")},
            {"output_dir", string("Absolute, outside the project. Default: last for mode.")},
            {"overwrite", withDefault(boolean("source: allow a non-empty output_dir."), false)},
            {"start_scene", sceneRef("Also becomes the project's start scene. Default: project's.")},
            {"graphics_api", string("desktop: opengl/vulkan/d3d11/metal-macos (per OS); web: opengles. Remembered.")},
            {"platform", stringEnum("source: shader target platform.", {"windows", "macos", "linux", "android", "ios", "web"})},
            {"backends", array(stringItem, "source: opengl/opengles/d3d11/metal-macos/metal-ios/vulkan/all. Remembered.")},
            {"build_type", withDefault(stringEnum("", {"Release", "Debug", "RelWithDebInfo", "MinSizeRel"}), "Release")},
            {"build_jobs", integer("desktop: 0 = auto. Default: preferences.build_jobs.")},
            {"shaders", object({
                {"add", array(stringItem, "Shader keys (get_build_info include_shaders).")},
                {"exclude", array(stringItem, "")},
                {"reset", boolean("Clear manual add/exclude first.")}
            })},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", jobTimeout}
        });
        tool.annotations.openWorld = true;
        tool.allowedWhilePlaying = false;
        tool.handler = buildProjectTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "generate_shaders";
        tool.title = "Generate Shaders";
        tool.description =
            "Compile shader variants into output_dir as a job, without a player (build_project does this itself). "
            "Default: the shaders the project's scenes need. format header = C++ headers, binary = .sdat.";
        tool.inputSchema = object({
            {"output_dir", string("Absolute.")},
            {"backends", array(stringItem, "Graphics APIs. Default: all.")},
            {"format", withDefault(stringEnum("", {"header", "binary", "json"}), "header")},
            {"shaders", array(stringItem, "Shader keys.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", jobTimeout}
        }, {"output_dir"});
        tool.annotations.openWorld = true;
        tool.handler = generateShadersTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "run_build";
        tool.title = "Run Build";
        tool.description =
            "Run a Desktop player for `seconds`, screenshot its window (Linux/X11), then stop it. Returns the "
            "log tail (full: log_file), errors (incl. Lua errors), exit code/signal, crashed, fatal_lines. "
            "input (Linux): send_input events into the player window: key, text, mouse (window pixels or space "
            "normalized), wait {ms|frames|seconds}, release_all, screenshot (an image then). screenshots_at: "
            "seconds since launch.";
        tool.inputSchema = object({
            {"path", string("Executable or build dir (absolute). Default: last desktop build.")},
            {"seconds", number("Default: 5, or past the input.", 0.5, 300)},
            {"input", Json{{"type", "array"}, {"maxItems", 500}, {"items", anyObject("")}}},
            {"input_delay_seconds", withDefault(number("", 0, 120), 1)},
            {"space", withDefault(stringEnum("", {"window", "normalized"}), "window")},
            {"gap_ms", withDefault(number("", 0, 10000), 50)},
            {"screenshots_at", array(number(""), "")},
            {"screenshot", boolean("Default: true unless other screenshots.")},
            {"screenshot_after_seconds", number("Default min(3, 0.8 * seconds).")},
            {"max_width", withDefault(integer("", 64, 4096), 1280)},
            {"args", array(stringItem, "Player arguments.")},
            {"log_lines", withDefault(integer("", 0, 2000), 80)}
        });
        tool.annotations.openWorld = true;
        tool.handler = runBuildTool;
        registry.addTool(std::move(tool));
    }
}

}

// ------------------------------------------------------------------ X11 (last: Xlib's macros clash with engine names)

#if defined(__linux__) && defined(JASPERENGINE_HAS_X11_CLIPBOARD)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dlfcn.h>

namespace jasperengine::editor::mcp {
namespace {

int xErrors = 0;
int recordXError(Display*, XErrorEvent*) {
    xErrors++;
    return 0;
}

// Runs fn with an X error handler that records instead of exiting; main thread only
template <typename Fn>
void withXErrorsTrapped(Display* display, Fn fn) {
    XSync(display, False);
    xErrors = 0;
    auto previous = XSetErrorHandler(recordXError);
    fn();
    XSync(display, False);
    XSetErrorHandler(previous);
}

std::vector<unsigned long> listTopLevelWindows() {
    std::vector<unsigned long> out;
    Display* display = XOpenDisplay(nullptr);
    if (!display) return out;
    withXErrorsTrapped(display, [&]() {
        Window root = DefaultRootWindow(display);
        Window rootReturn, parentReturn;
        Window* children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &count)) {
            for (unsigned int i = 0; i < count; i++) out.push_back(children[i]);
            if (children) XFree(children);
        }
    });
    XCloseDisplay(display);
    return out;
}

int maskShift(unsigned long mask) {
    int shift = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

int maskBits(unsigned long mask) {
    int bits = 0;
    mask >>= maskShift(mask);
    while (mask & 1) {
        mask >>= 1;
        bits++;
    }
    return bits;
}

ScreenCapture captureWindow(unsigned long window, int maxWidth) {
    ScreenCapture capture;
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        capture.note = "no X display (DISPLAY unset)";
        return capture;
    }
    withXErrorsTrapped(display, [&]() {
        Window root = DefaultRootWindow(display);
        XWindowAttributes rootAttr;
        if (!XGetWindowAttributes(display, root, &rootAttr)) {
            capture.note = "could not read the screen";
            return;
        }
        int x = 0, y = 0, w = rootAttr.width, h = rootAttr.height;
        Window found = 0;
        XWindowAttributes attr;
        if (window && XGetWindowAttributes(display, window, &attr) && attr.map_state == IsViewable) {
            Window child;
            int ax = 0, ay = 0;
            XTranslateCoordinates(display, window, root, 0, 0, &ax, &ay, &child);
            x = ax;
            y = ay;
            w = attr.width;
            h = attr.height;
            found = window;
        }
        // clip to the screen (XGetImage of the root fails outside it)
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(rootAttr.width, x + w), y1 = std::min(rootAttr.height, y + h);
        if (x1 <= x0 || y1 <= y0) {
            capture.note = "the player window is off screen";
            return;
        }
        w = x1 - x0;
        h = y1 - y0;
        XImage* image = XGetImage(display, root, x0, y0, static_cast<unsigned>(w), static_cast<unsigned>(h), AllPlanes, ZPixmap);
        if (!image) {
            capture.note = "XGetImage failed";
            return;
        }
        std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
        const int rs = maskShift(image->red_mask), gs = maskShift(image->green_mask), bs = maskShift(image->blue_mask);
        const int rb = maskBits(image->red_mask), gb = maskBits(image->green_mask), bb = maskBits(image->blue_mask);
        auto scale = [](unsigned long v, int bits) -> unsigned char {
            if (bits <= 0) return 0;
            if (bits >= 8) return static_cast<unsigned char>(v >> (bits - 8));
            return static_cast<unsigned char>((v * 255) / ((1u << bits) - 1));
        };
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                const unsigned long pixel = XGetPixel(image, col, row);
                unsigned char* out = &rgb[(static_cast<size_t>(row) * w + col) * 3];
                out[0] = scale((pixel & image->red_mask) >> rs, rb);
                out[1] = scale((pixel & image->green_mask) >> gs, gb);
                out[2] = scale((pixel & image->blue_mask) >> bs, bb);
            }
        }
        XDestroyImage(image);

        int finalW = w, finalH = h;
        std::vector<unsigned char> scaled;
        const unsigned char* pixels = rgb.data();
        if (maxWidth > 0 && w > maxWidth) {
            finalW = maxWidth;
            finalH = std::max(1, static_cast<int>(static_cast<long long>(h) * maxWidth / w));
            scaled.resize(static_cast<size_t>(finalW) * finalH * 3);
            if (stbir_resize_uint8_srgb(rgb.data(), w, h, 0, scaled.data(), finalW, finalH, 0, STBIR_RGB)) {
                pixels = scaled.data();
            } else {
                finalW = w;
                finalH = h;
            }
        }
        auto append = [](void* context, void* data, int size) {
            auto* vec = static_cast<std::vector<uint8_t>*>(context);
            const auto* bytes = static_cast<const uint8_t*>(data);
            vec->insert(vec->end(), bytes, bytes + size);
        };
        if (!stbi_write_png_to_func(append, &capture.png, finalW, finalH, 3, pixels, finalW * 3)) {
            capture.note = "PNG encoding failed";
            capture.png.clear();
            return;
        }
        capture.ok = true;
        capture.width = finalW;
        capture.height = finalH;
        capture.note = found ? "the player window (" + std::to_string(w) + "x" + std::to_string(h) + " at " +
                                   std::to_string(x0) + "," + std::to_string(y0) + ")"
                             : "the whole screen (no player window was found: the player may not have opened one)";
    });
    if (xErrors && !capture.ok && capture.note.empty()) capture.note = "X error while capturing";
    XCloseDisplay(display);
    return capture;
}

// The _NET_WM_PID of a window, -1 if it has none
long windowPid(Display* display, Window window) {
    static Atom pidAtom = 0;
    if (!pidAtom) pidAtom = XInternAtom(display, "_NET_WM_PID", True);
    if (!pidAtom) return -1;
    Atom type;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    long pid = -1;
    if (XGetWindowProperty(display, window, pidAtom, 0, 1, False, XA_CARDINAL, &type, &format, &count, &after, &data) == Success) {
        if (data && count == 1 && format == 32) pid = static_cast<long>(*reinterpret_cast<unsigned long*>(data));
        if (data) XFree(data);
    }
    return pid;
}

// Depth-first: the first viewable window of pid under parent (a window manager reparents)
Window findPidWindow(Display* display, Window parent, long pid, int depth) {
    Window rootReturn, parentReturn;
    Window* children = nullptr;
    unsigned int count = 0;
    Window found = 0;
    if (!XQueryTree(display, parent, &rootReturn, &parentReturn, &children, &count)) return 0;
    for (int i = static_cast<int>(count) - 1; i >= 0 && !found; i--) {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, children[i], &attr) || attr.map_state != IsViewable) continue;
        if (windowPid(display, children[i]) == pid && attr.width >= 16 && attr.height >= 16) found = children[i];
        else if (depth < 2) found = findPidWindow(display, children[i], pid, depth + 1);
    }
    if (children) XFree(children);
    return found;
}

unsigned long findPlayerWindow(long pid, const std::vector<unsigned long>& before) {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return 0;
    Window found = 0;
    withXErrorsTrapped(display, [&]() {
        Window root = DefaultRootWindow(display);
        found = findPidWindow(display, root, pid, 0);
        if (!found) {
            // a player that does not set _NET_WM_PID: the topmost new window
            Window rootReturn, parentReturn;
            Window* children = nullptr;
            unsigned int count = 0;
            if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &count)) {
                for (int i = static_cast<int>(count) - 1; i >= 0 && !found; i--) {
                    if (std::find(before.begin(), before.end(), children[i]) != before.end()) continue;
                    XWindowAttributes attr;
                    if (!XGetWindowAttributes(display, children[i], &attr)) continue;
                    if (attr.map_state != IsViewable || attr.width < 16 || attr.height < 16 || attr.c_class == InputOnly) continue;
                    found = children[i];
                }
                if (children) XFree(children);
            }
        }
        // on top of the editor (same display), so screen reads and the pointer reach it
        if (found) XRaiseWindow(display, found);
    });
    XCloseDisplay(display);
    return found;
}

bool playerWindowRect(unsigned long window, int& x, int& y, int& width, int& height) {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    bool ok = false;
    withXErrorsTrapped(display, [&]() {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, window, &attr)) return;
        Window child;
        XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &x, &y, &child);
        width = attr.width;
        height = attr.height;
        ok = true;
    });
    XCloseDisplay(display);
    return ok;
}

// ------------------------------------------------------------------ input: XTest (libXtst, loaded at run time), else XSendEvent

using FakeKeyFn = int (*)(Display*, unsigned int, int, unsigned long);
using FakeButtonFn = int (*)(Display*, unsigned int, int, unsigned long);
using FakeMotionFn = int (*)(Display*, int, int, int, unsigned long);

struct XTestApi {
    bool tried = false;
    FakeKeyFn key = nullptr;
    FakeButtonFn button = nullptr;
    FakeMotionFn motion = nullptr;
};

XTestApi& xtest() {
    static XTestApi api;
    if (!api.tried) {
        api.tried = true;
        void* lib = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libXtst.so", RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            api.key = reinterpret_cast<FakeKeyFn>(dlsym(lib, "XTestFakeKeyEvent"));
            api.button = reinterpret_cast<FakeButtonFn>(dlsym(lib, "XTestFakeButtonEvent"));
            api.motion = reinterpret_cast<FakeMotionFn>(dlsym(lib, "XTestFakeMotionEvent"));
            if (!api.key || !api.button || !api.motion) api.key = nullptr;
        }
    }
    return api;
}

struct PlayerInputSender {
    Display* display = nullptr;
    bool useXTest = false;
    bool focused = false;
    int pointerX = 0, pointerY = 0;          // window pixels
    std::set<unsigned int> keysDown;          // keycodes
    std::set<unsigned int> buttonsDown;
};

std::string playerInputUnsupported() {
    return std::string();
}

unsigned long playerKeysym(const std::string& rawName) {
    std::string name = rawName;
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return XK_a + (c - 'a');
        if (c >= '0' && c <= '9') return XK_0 + (c - '0');
        bool shift = false;
        return playerCharKeysym(static_cast<unsigned char>(c), shift);
    }
    for (char& c : name) if (c == ' ' || c == '-') c = '_';
    if (name.rfind("key_", 0) == 0) name = name.substr(4);
    static const std::map<std::string, unsigned long> keys = {
        {"space", XK_space}, {"spacebar", XK_space}, {"escape", XK_Escape}, {"esc", XK_Escape},
        {"enter", XK_Return}, {"return", XK_Return}, {"tab", XK_Tab}, {"backspace", XK_BackSpace},
        {"insert", XK_Insert}, {"delete", XK_Delete}, {"del", XK_Delete},
        {"left", XK_Left}, {"right", XK_Right}, {"up", XK_Up}, {"down", XK_Down},
        {"left_arrow", XK_Left}, {"right_arrow", XK_Right}, {"up_arrow", XK_Up}, {"down_arrow", XK_Down},
        {"arrow_left", XK_Left}, {"arrow_right", XK_Right}, {"arrow_up", XK_Up}, {"arrow_down", XK_Down},
        {"page_up", XK_Page_Up}, {"pageup", XK_Page_Up}, {"page_down", XK_Page_Down}, {"pagedown", XK_Page_Down},
        {"home", XK_Home}, {"end", XK_End}, {"caps_lock", XK_Caps_Lock}, {"num_lock", XK_Num_Lock},
        {"scroll_lock", XK_Scroll_Lock}, {"print_screen", XK_Print}, {"pause", XK_Pause}, {"menu", XK_Menu},
        {"shift", XK_Shift_L}, {"left_shift", XK_Shift_L}, {"right_shift", XK_Shift_R},
        {"ctrl", XK_Control_L}, {"control", XK_Control_L}, {"left_ctrl", XK_Control_L}, {"left_control", XK_Control_L},
        {"right_ctrl", XK_Control_R}, {"right_control", XK_Control_R},
        {"alt", XK_Alt_L}, {"left_alt", XK_Alt_L}, {"right_alt", XK_Alt_R},
        {"super", XK_Super_L}, {"left_super", XK_Super_L}, {"right_super", XK_Super_R},
        {"apostrophe", XK_apostrophe}, {"comma", XK_comma}, {"minus", XK_minus}, {"period", XK_period},
        {"slash", XK_slash}, {"semicolon", XK_semicolon}, {"equal", XK_equal}, {"equals", XK_equal},
        {"left_bracket", XK_bracketleft}, {"backslash", XK_backslash}, {"right_bracket", XK_bracketright},
        {"grave_accent", XK_grave}, {"backquote", XK_grave},
        {"kp_decimal", XK_KP_Decimal}, {"kp_divide", XK_KP_Divide}, {"kp_multiply", XK_KP_Multiply},
        {"kp_subtract", XK_KP_Subtract}, {"kp_add", XK_KP_Add}, {"kp_enter", XK_KP_Enter}, {"kp_equal", XK_KP_Equal}
    };
    auto it = keys.find(name);
    if (it != keys.end()) return it->second;
    if (name.size() >= 2 && name[0] == 'f' && std::all_of(name.begin() + 1, name.end(), ::isdigit)) {
        const int n = std::stoi(name.substr(1));
        if (n >= 1 && n <= 24) return XK_F1 + (n - 1);
    }
    if ((name.rfind("kp_", 0) == 0 && name.size() == 4 && std::isdigit(static_cast<unsigned char>(name[3])))) {
        return XK_KP_0 + (name[3] - '0');
    }
    return 0;
}

unsigned long playerCharKeysym(uint32_t cp, bool& shift) {
    shift = false;
    if (cp >= 'a' && cp <= 'z') return XK_a + (cp - 'a');
    if (cp >= 'A' && cp <= 'Z') {
        shift = true;
        return XK_a + (cp - 'A');
    }
    if (cp >= '0' && cp <= '9') return XK_0 + (cp - '0');
    static const std::map<uint32_t, std::pair<unsigned long, bool>> chars = {
        {' ', {XK_space, false}}, {'\n', {XK_Return, false}}, {'\t', {XK_Tab, false}},
        {'-', {XK_minus, false}}, {'=', {XK_equal, false}}, {'[', {XK_bracketleft, false}}, {']', {XK_bracketright, false}},
        {';', {XK_semicolon, false}}, {'\'', {XK_apostrophe, false}}, {',', {XK_comma, false}}, {'.', {XK_period, false}},
        {'/', {XK_slash, false}}, {'\\', {XK_backslash, false}}, {'`', {XK_grave, false}},
        {'!', {XK_1, true}}, {'@', {XK_2, true}}, {'#', {XK_3, true}}, {'$', {XK_4, true}}, {'%', {XK_5, true}},
        {'^', {XK_6, true}}, {'&', {XK_7, true}}, {'*', {XK_8, true}}, {'(', {XK_9, true}}, {')', {XK_0, true}},
        {'_', {XK_minus, true}}, {'+', {XK_equal, true}}, {'{', {XK_bracketleft, true}}, {'}', {XK_bracketright, true}},
        {':', {XK_semicolon, true}}, {'"', {XK_apostrophe, true}}, {'<', {XK_comma, true}}, {'>', {XK_period, true}},
        {'?', {XK_slash, true}}, {'|', {XK_backslash, true}}, {'~', {XK_grave, true}}
    };
    auto it = chars.find(cp);
    if (it == chars.end()) return 0;
    shift = it->second.second;
    return it->second.first;
}

PlayerInputSender* openPlayerInput(std::string& method, std::string& error) {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        error = "no X display (DISPLAY unset)";
        return nullptr;
    }
    auto* sender = new PlayerInputSender();
    sender->display = display;
    sender->useXTest = xtest().key != nullptr;
    method = sender->useXTest ? "xtest" : "xsendevent";
    return sender;
}

// The window's top-left on the root window
void windowOrigin(Display* display, Window window, int& x, int& y) {
    Window child;
    x = y = 0;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &x, &y, &child);
}

// XSendEvent fallback: a synthetic event to the window (some toolkits ignore those)
void sendSynthetic(PlayerInputSender* s, Window window, int type, unsigned int detail) {
    Display* d = s->display;
    int ox = 0, oy = 0;
    windowOrigin(d, window, ox, oy);
    XEvent event;
    std::memset(&event, 0, sizeof(event));
    unsigned int state = 0;
    for (unsigned int b : s->buttonsDown) if (b >= 1 && b <= 5) state |= (Button1Mask << (b - 1));
    if (type == KeyPress || type == KeyRelease) {
        XKeyEvent& e = event.xkey;
        e.type = type;
        e.display = d;
        e.window = window;
        e.root = DefaultRootWindow(d);
        e.time = CurrentTime;
        e.x = s->pointerX;
        e.y = s->pointerY;
        e.x_root = ox + s->pointerX;
        e.y_root = oy + s->pointerY;
        e.keycode = detail;
        e.state = state;
        e.same_screen = True;
        XSendEvent(d, window, True, type == KeyPress ? KeyPressMask : KeyReleaseMask, &event);
    } else if (type == ButtonPress || type == ButtonRelease) {
        XButtonEvent& e = event.xbutton;
        e.type = type;
        e.display = d;
        e.window = window;
        e.root = DefaultRootWindow(d);
        e.time = CurrentTime;
        e.x = s->pointerX;
        e.y = s->pointerY;
        e.x_root = ox + s->pointerX;
        e.y_root = oy + s->pointerY;
        e.button = detail;
        e.state = state;
        e.same_screen = True;
        XSendEvent(d, window, True, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &event);
    } else {
        XMotionEvent& e = event.xmotion;
        e.type = MotionNotify;
        e.display = d;
        e.window = window;
        e.root = DefaultRootWindow(d);
        e.time = CurrentTime;
        e.x = s->pointerX;
        e.y = s->pointerY;
        e.x_root = ox + s->pointerX;
        e.y_root = oy + s->pointerY;
        e.state = state;
        e.same_screen = True;
        XSendEvent(d, window, True, PointerMotionMask, &event);
    }
}

std::string sendPlayerAction(PlayerInputSender* s, unsigned long window, const PlayerAction& action) {
    if (!s || !window) return "no player window";
    Display* d = s->display;
    std::string error;
    withXErrorsTrapped(d, [&]() {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(d, window, &attr)) {
            error = "the player window is gone";
            return;
        }
        if (!s->focused) {
            // keys go to the focused window, the pointer to the one under it
            XRaiseWindow(d, window);
            XSetInputFocus(d, window, RevertToParent, CurrentTime);
            s->focused = true;
        }
        const XTestApi& api = xtest();
        switch (action.kind) {
            case PlayerAction::Move: {
                int px = static_cast<int>(std::lround(action.normalized ? action.x * attr.width : action.x));
                int py = static_cast<int>(std::lround(action.normalized ? action.y * attr.height : action.y));
                if (px < 0 || py < 0 || px >= attr.width || py >= attr.height) {
                    error = "point (" + std::to_string(px) + ", " + std::to_string(py) + ") is outside the player window (" +
                            std::to_string(attr.width) + "x" + std::to_string(attr.height) + ")";
                    return;
                }
                s->pointerX = px;
                s->pointerY = py;
                if (s->useXTest) {
                    int ox = 0, oy = 0;
                    windowOrigin(d, window, ox, oy);
                    api.motion(d, DefaultScreen(d), ox + px, oy + py, CurrentTime);
                } else {
                    sendSynthetic(s, window, MotionNotify, 0);
                }
                break;
            }
            case PlayerAction::KeyDown:
            case PlayerAction::KeyUp: {
                const unsigned int code = XKeysymToKeycode(d, static_cast<KeySym>(action.keysym));
                if (!code) {
                    error = "the X keyboard has no key for keysym " + std::to_string(action.keysym);
                    return;
                }
                const bool down = action.kind == PlayerAction::KeyDown;
                if (s->useXTest) api.key(d, code, down ? True : False, CurrentTime);
                else sendSynthetic(s, window, down ? KeyPress : KeyRelease, code);
                if (down) s->keysDown.insert(code);
                else s->keysDown.erase(code);
                break;
            }
            case PlayerAction::ButtonDown:
            case PlayerAction::ButtonUp: {
                const bool down = action.kind == PlayerAction::ButtonDown;
                if (s->useXTest) api.button(d, static_cast<unsigned int>(action.button), down ? True : False, CurrentTime);
                else sendSynthetic(s, window, down ? ButtonPress : ButtonRelease, static_cast<unsigned int>(action.button));
                if (down) s->buttonsDown.insert(static_cast<unsigned int>(action.button));
                else s->buttonsDown.erase(static_cast<unsigned int>(action.button));
                break;
            }
            case PlayerAction::ReleaseAll: {
                for (unsigned int code : s->keysDown) {
                    if (s->useXTest) api.key(d, code, False, CurrentTime);
                    else sendSynthetic(s, window, KeyRelease, code);
                }
                for (unsigned int b : s->buttonsDown) {
                    if (s->useXTest) api.button(d, b, False, CurrentTime);
                    else sendSynthetic(s, window, ButtonRelease, b);
                }
                s->keysDown.clear();
                s->buttonsDown.clear();
                break;
            }
            case PlayerAction::Screenshot:
                break;
        }
        XFlush(d);
    });
    return error;
}

void closePlayerInput(PlayerInputSender* s, unsigned long window) {
    if (!s) return;
    if (window && (!s->keysDown.empty() || !s->buttonsDown.empty())) {
        PlayerAction release;
        release.kind = PlayerAction::ReleaseAll;
        sendPlayerAction(s, window, release);
    } else if (s->useXTest) {
        // XTest keys are global: never leave one down on the display
        const XTestApi& api = xtest();
        for (unsigned int code : s->keysDown) api.key(s->display, code, False, CurrentTime);
        for (unsigned int b : s->buttonsDown) api.button(s->display, b, False, CurrentTime);
        XFlush(s->display);
    }
    XCloseDisplay(s->display);
    delete s;
}

}
}

#else

namespace jasperengine::editor::mcp {
namespace {

std::vector<unsigned long> listTopLevelWindows() {
    return {};
}

unsigned long findPlayerWindow(long, const std::vector<unsigned long>&) {
    return 0;
}

ScreenCapture captureWindow(unsigned long, int) {
    ScreenCapture capture;
    capture.note = "screenshots of a running player need X11 (Linux); only the log is available here";
    return capture;
}

bool playerWindowRect(unsigned long, int&, int&, int&, int&) {
    return false;
}

struct PlayerInputSender {};

std::string playerInputUnsupported() {
    return "input injection into a built player needs X11 (Linux)";
}

unsigned long playerKeysym(const std::string&) {
    return 0;
}

unsigned long playerCharKeysym(uint32_t, bool& shift) {
    shift = false;
    return 0;
}

PlayerInputSender* openPlayerInput(std::string&, std::string& error) {
    error = playerInputUnsupported();
    return nullptr;
}

std::string sendPlayerAction(PlayerInputSender*, unsigned long, const PlayerAction&) {
    return playerInputUnsupported();
}

void closePlayerInput(PlayerInputSender*, unsigned long) {}

}
}

#endif
