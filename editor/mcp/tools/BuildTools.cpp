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
#include <chrono>
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
// The topmost viewable top-level window not in `before`, read from the screen; with none,
// the whole screen. maxWidth scales the image down.
ScreenCapture captureNewWindow(const std::vector<unsigned long>& before, int maxWidth);

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

// Lines of a log that mean the program failed (tools/smoke-test.sh fatal_lines)
bool isFatalLine(const std::string& line) {
    static const std::regex fatal(R"(\((ERROR|PANIC)\)|\[ERROR\]|Lua Error|[Mm]issing (precompiled )?shader|[Aa]ssertion|Aborted|Segmentation fault|terminate called|core dumped|AddressSanitizer|stack smashing)");
    if (line.find("Failed to initialize audio output") != std::string::npos) return false;
    return std::regex_search(line, fatal);
}

#ifndef _WIN32

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
    double screenshotAfter = -1.0;   // < 0: none
    bool screenshotDone = false;
    int maxWidth = 1280;
    std::vector<unsigned long> windowsBefore;
    ScreenCapture capture;
    fs::path exe;
    fs::path logPath;
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

void finishPlayerRun(const CallPtr& call, const std::shared_ptr<PlayerRun>& run) {
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
    for (const std::string& line : lines) {
        if (isFatalLine(line) && fatal.size() < 30) fatal.push_back(line);
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
    if (!fatal.empty()) text += ", " + std::to_string(fatal.size()) + " fatal: " + fatal[0].get<std::string>();
    text += ".";
    if (run->capture.ok) {
        result.images.push_back({Base64::encode(run->capture.png.data(), run->capture.png.size()), "image/png"});
        data["screenshot"] = {{"width", run->capture.width}, {"height", run->capture.height}, {"note", run->capture.note}};
        text += " Screenshot: " + run->capture.note + ".";
    } else if (run->screenshotAfter >= 0.0) {
        data["screenshot"] = nullptr;
        data["screenshot_error"] = run->capture.note.empty() ? "no screenshot taken (the player exited first)" : run->capture.note;
        text += " No screenshot: " + data["screenshot_error"].get<std::string>() + ".";
    }
    if (!tail.empty()) {
        text += "\nLast log lines:";
        const size_t shown = std::min<size_t>(tail.size(), 15);
        for (size_t i = tail.size() - shown; i < tail.size(); i++) text += "\n  " + tail[i].get<std::string>();
    }
    result.text = text;
    result.structured = data;
    result.isError = false;   // a crash is a finding about the game, not a tool failure
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
    run->seconds = std::max(0.5, std::min(300.0, args.value("seconds", 5.0)));
    const bool wantScreenshot = args.value("screenshot", true);
    if (wantScreenshot) {
        double after = args.value("screenshot_after_seconds", std::min(3.0, run->seconds * 0.8));
        run->screenshotAfter = std::max(0.1, std::min(after, run->seconds));
        run->windowsBefore = listTopLevelWindows();
    }
    run->maxWidth = std::max(64, std::min(4096, args.value("max_width", 1280)));
    {
        const fs::path logs = project->getProjectInternalPath() / "logs";
        fs::create_directories(logs, ec);
        static std::atomic<unsigned> counter{0};
        run->logPath = logs / ("run-" + std::to_string(++counter) + ".log");
    }

    std::vector<std::string> argStrings = {exe.string()};
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
            }
        }
        const double elapsed = secondsSince(run->started);
        if (!run->exited && !run->screenshotDone && run->screenshotAfter >= 0.0 && elapsed >= run->screenshotAfter) {
            run->screenshotDone = true;
            run->capture = captureNewWindow(run->windowsBefore, run->maxWidth);
        }
        if (run->exited) {
            // the rest of the output: wait for EOF briefly (a child of the player may hold the pipe)
            return run->fd < 0 || secondsSince(run->exitedAt) > 1.0;
        }
        if (!run->terminating && elapsed >= run->seconds) {
            run->terminating = true;
            run->killedByUs = true;
            run->terminateAt = std::chrono::steady_clock::now();
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

    registry.removeTool("export_project");

    {
        ToolDef tool;
        tool.name = "get_build_info";
        tool.title = "Get Build Info";
        tool.description =
            "Build Settings state: what this computer can build (desktop: cmake + C++ compiler found, missing tools listed; "
            "web: Emscripten SDK found), the graphics APIs per mode, the remembered output directory per mode, the "
            "compiler kit and job count, the scenes in the build and start scene, the shader count (include_shaders: "
            "true lists each shader's key and name for build_project shaders.add / exclude), the running build job and "
            "the last build's result and executable. Call before build_project.";
        tool.inputSchema = object({
            {"include_shaders", withDefault(boolean("List every shader variant (key, name, from_scenes, excluded)."), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getBuildInfoTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "build_project";
        tool.title = "Build Project";
        tool.description =
            "File > Build Settings > Build, as a job. mode desktop (default) compiles a runnable player for this computer "
            "(cmake + C++ compiler; the engine is compiled from source, so a first build takes many minutes, later ones are "
            "incremental); web builds a WebAssembly player (needs emsdk); source exports the standalone C++/CMake project "
            "for any platform (platform or backends pick the shader graphics APIs). Every scene is saved first; refused "
            "during Play. Returns when done or after timeout_seconds with {job_id, status: running}: poll get_job, stop "
            "with cancel_job. The result has output_dir, artifacts, executable (desktop), errors [{file, line, column, "
            "message, project_file}] parsed from the compiler/CMake output, warnings, and log_file with the whole log. "
            "The exporter reads the project from its own thread: don't edit scenes or start Play while a build job runs "
            "(project tools refuse to). Verify a desktop player with run_build. Example: {\"mode\": \"desktop\", \"output_dir\": \"/home/me/builds/game\", "
            "\"build_jobs\": 4}.";
        tool.inputSchema = object({
            {"mode", withDefault(stringEnum("What to build.", {"desktop", "web", "source"}), "desktop")},
            {"output_dir", string("Absolute output directory outside the project. Default: the one used last for "
                                  "this mode (get_build_info), else <project>/.jasperengine/builds/<mode>.")},
            {"overwrite", withDefault(boolean("source mode: export into a non-empty output_dir (same-name files are replaced)."), false)},
            {"start_scene", sceneRef("Start scene (saved scene id, name or path); also becomes the project's start scene. Default: the project's.")},
            {"graphics_api", string("desktop: opengl or vulkan on Linux (opengl, vulkan, d3d11 on Windows; metal-macos, opengl on macOS); remembered. web: opengles.")},
            {"platform", stringEnum("source mode: target platform whose graphics APIs the shaders are compiled for.",
                                    {"windows", "macos", "linux", "android", "ios", "web"})},
            {"backends", array(Json{{"type", "string"}}, "source mode: explicit graphics APIs (opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, or all); remembered.")},
            {"build_type", withDefault(stringEnum("desktop/web CMake build type.", {"Release", "Debug", "RelWithDebInfo", "MinSizeRel"}), "Release")},
            {"build_jobs", integer("desktop: parallel compile jobs (0 = automatic). Default: the project's (preferences.build_jobs).")},
            {"shaders", object({
                {"add", array(Json{{"type", "string"}}, "Shader keys to include (get_build_info include_shaders).")},
                {"exclude", array(Json{{"type", "string"}}, "Shader keys to leave out.")},
                {"reset", boolean("Clear the manual additions/exclusions first.")}
            })},
            {"wait", withDefault(boolean("Wait for the build (up to timeout_seconds)."), true)},
            {"timeout_seconds", withDefault(number("How long to wait before returning the running job.", 0, 3600), 120)}
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
            "Compile shader variants into output_dir as a job (what a build embeds), without building a player: by default "
            "the shaders the project's scenes need (with the build's manual additions/exclusions), or the given shader "
            "keys. format header (C++ headers), binary (.sdat) or json. Rarely needed: build_project compiles shaders itself.";
        tool.inputSchema = object({
            {"output_dir", string("Absolute output directory.")},
            {"backends", array(Json{{"type", "string"}}, "Graphics APIs (opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all). Default all.")},
            {"format", withDefault(stringEnum("Output format.", {"header", "binary", "json"}), "header")},
            {"shaders", array(Json{{"type", "string"}}, "Shader keys (get_build_info include_shaders). Default: the project's.")},
            {"wait", withDefault(boolean("Wait for it (up to timeout_seconds)."), true)},
            {"timeout_seconds", withDefault(number("How long to wait before returning the running job.", 0, 3600), 120)}
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
            "Launch a built Desktop player (default: the last one build_project made) as a child process on the editor's "
            "display, let it run for `seconds`, take a screenshot of its window after screenshot_after_seconds (Linux/X11; "
            "returned as an image), then stop it. Returns its stdout/stderr log (last log_lines lines, the whole log in "
            "log_file), exit code or signal if it exited by itself (exited_early), crashed (exited with an error/signal or "
            "logged fatal lines: errors, Lua errors, missing shaders, assertions, segfaults), and fatal_lines. Use it to "
            "check that a shipped game starts and shows what it should. Not available on Windows yet.";
        tool.inputSchema = object({
            {"path", string("Absolute player executable, or a build output directory. Default: the last Desktop build.")},
            {"seconds", withDefault(number("How long to let it run before stopping it.", 0.5, 300), 5)},
            {"screenshot", withDefault(boolean("Capture its window."), true)},
            {"screenshot_after_seconds", number("When to capture (default min(3, 0.8 * seconds)).")},
            {"max_width", withDefault(integer("Scale the screenshot down to this width.", 64, 4096), 1280)},
            {"args", array(Json{{"type", "string"}}, "Command-line arguments for the player.")},
            {"log_lines", withDefault(integer("How many of the last log lines to return.", 0, 2000), 80)}
        });
        tool.annotations.openWorld = true;
        tool.handler = runBuildTool;
        registry.addTool(std::move(tool));
    }
}

}

// ------------------------------------------------------------------ X11 (last: Xlib's macros clash with engine names)

#if defined(__linux__) && defined(JASPERENGINE_HAS_X11_CLIPBOARD)

#include <X11/Xlib.h>
#include <X11/Xutil.h>

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

ScreenCapture captureNewWindow(const std::vector<unsigned long>& before, int maxWidth) {
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
        // topmost new viewable window
        int x = 0, y = 0, w = rootAttr.width, h = rootAttr.height;
        Window found = 0;
        Window rootReturn, parentReturn;
        Window* children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &count)) {
            for (int i = static_cast<int>(count) - 1; i >= 0 && !found; i--) {
                const unsigned long id = children[i];
                if (std::find(before.begin(), before.end(), id) != before.end()) continue;
                XWindowAttributes attr;
                if (!XGetWindowAttributes(display, children[i], &attr)) continue;
                if (attr.map_state != IsViewable || attr.width < 16 || attr.height < 16 || attr.c_class == InputOnly) continue;
                Window child;
                int ax = 0, ay = 0;
                XTranslateCoordinates(display, children[i], root, 0, 0, &ax, &ay, &child);
                x = ax;
                y = ay;
                w = attr.width;
                h = attr.height;
                found = children[i];
            }
            if (children) XFree(children);
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
                             : "the whole screen (no new window was found: the player may not have opened one)";
    });
    if (xErrors && !capture.ok && capture.note.empty()) capture.note = "X error while capturing";
    XCloseDisplay(display);
    return capture;
}

}
}

#else

namespace jasperengine::editor::mcp {
namespace {

std::vector<unsigned long> listTopLevelWindows() {
    return {};
}

ScreenCapture captureNewWindow(const std::vector<unsigned long>&, int) {
    ScreenCapture capture;
    capture.note = "screenshots of a running player need X11 (Linux); only the log is available here";
    return capture;
}

}
}

#endif
