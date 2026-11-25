// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShippingCommands.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Exporter.h"
#include "JasperEngine.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "cli/CommandLine.h"
#include "cook/BuildReport.h"
#include "cook/RuntimeFieldsEmitter.h"
#include "io/AtomicFile.h"
#include "platform/ExternalTools.h"
#include "mcp/tools/BuildDevices.h"
#include "platform/DeviceRunner.h"
#include "players/PlayerRuntimes.h"
#include "pool/ShaderPool.h"
#include "publish/Publishing.h"
#include "shader/ShaderBuilder.h"

#include "json.hpp"
#include "util/FileUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <fcntl.h>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace {

    // the GUI-subsystem editor on Windows gets its parent's console back for a CLI command
    void attachConsole() {
#if defined(_WIN32)
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* out = nullptr;
            FILE* err = nullptr;
            (void)freopen_s(&out, "CONOUT$", "w", stdout);
            (void)freopen_s(&err, "CONOUT$", "w", stderr);
        }
#endif
    }

    // While a build runs, the engine's and exporter's log lines go to stderr: stdout carries only
    // the result (`key: value` lines or --json)
    class StdoutToStderr {
    public:
        StdoutToStderr() {
            std::cout.flush();
            std::fflush(stdout);
#if defined(_WIN32)
            saved = _dup(1);
            const int target = _dup(2);
            if (target >= 0) {
                _dup2(target, 1);
                _close(target);
            }
#else
            saved = dup(1);
            const int target = dup(2);
            if (target >= 0) {
                dup2(target, 1);
                close(target);
            }
#endif
        }
        ~StdoutToStderr() { restore(); }
        void restore() {
            std::cout.flush();
            std::fflush(stdout);
            if (saved < 0) return;
#if defined(_WIN32)
            _dup2(saved, 1);
            _close(saved);
#else
            dup2(saved, 1);
            close(saved);
#endif
            saved = -1;
        }
        StdoutToStderr(const StdoutToStderr&) = delete;
        StdoutToStderr& operator=(const StdoutToStderr&) = delete;

    private:
        int saved = -1;
    };

    struct EngineRuntimeGuard {
        Project& project;
        ~EngineRuntimeGuard() {
            // the project's scenes and their renders go while the engine runs: ~Project, which
            // comes after this guard, leaves them to the process exit
            project.releaseScenes();
            ShaderPool::setShaderBuilder(nullptr);
            ShaderBuilder::requestShutdown();
            Engine::systemShutdown();
        }
    };

    struct EditorHostOverrideGuard {
        EditorHost* previousHost = nullptr;
        explicit EditorHostOverrideGuard(EditorHost* host): previousHost(&getEditorHost()) { setEditorHost(host); }
        ~EditorHostOverrideGuard() { setEditorHost(previousHost); }
    };

    bool takeValue(int argc, char** argv, int& i, std::string& value, const std::string& command) {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s: %s needs a value\n", command.c_str(), argv[i]);
            return false;
        }
        value = argv[++i];
        return true;
    }

    std::string envOr(const char* name, const std::string& fallback = "") {
        const char* value = std::getenv(name);
        return value && *value ? std::string(value) : fallback;
    }

    // the scene of --start-scene: an id, or a scene's name or file
    uint32_t sceneIdOf(const Project& project, const std::string& requested) {
        if (requested.empty()) return 0;
        char* end = nullptr;
        const unsigned long id = std::strtoul(requested.c_str(), &end, 10);
        if (end && *end == '\0' && id > 0) return static_cast<uint32_t>(id);
        for (const SceneProject& scene : project.getScenes())
            if (scene.name == requested || scene.filepath.stem().string() == requested || scene.filepath.generic_string() == requested)
                return scene.id;
        return 0;
    }

    void printBuildUsage(const char* executableName) {
        std::printf(
            "Usage: %s build --project P --platform <windows|linux|macos|android|ios|web> --out D [options]\n"
            "       %s build --project P --profile <name> [--out D] [options]\n"
            "Builds a game: with a prebuilt player when the project needs no native code, else from source (S30).\n"
            "  --profile name           a Build Profile (EditorSettings/BuildProfiles): its platform, backend,\n"
            "                           flavour and Output Folder; a --platform that differs exits 2\n"
            "  --backend b              glcore, gles3, vulkan, d3d11 or metal (default: the platform's first)\n"
            "  --method m               auto (default: the project's), prebuilt or compile\n"
            "  --development            the development flavour (the profiler, --profile)\n"
            "  --pack                   the content in content.jpak\n"
            "  --android-format f       aab (Google Play: one ABI per device), apk-per-abi, or apk (one\n"
            "                           universal APK of every ABI). Default: Project Settings > Android\n"
            "  --start-scene s          the scene the game starts with (id or name)\n"
            "  --players-dir D          where the player runtimes are (also JASPER_PLAYERS_DIR)\n"
            "  --allow-player-mismatch  a player of another engine version (engine development only)\n"
            "  --ios-simulator          an unsigned simulator .app instead of an archive\n"
            "  --allow-missing-assets   build even when a scene references a missing asset\n"
            "  --run-device d           Build And Run on a device: an adb serial or simulator UDID, auto (the\n"
            "                           only usable one) or web; a Development Build patches the installed app\n"
            "  --no-patch               with --run-device: a full install, never Patch And Run\n"
            "  --run-seconds S          with --run-device: follow its log for S seconds (default 0: return\n"
            "                           once it launched; web: serve the page until interrupted)\n"
            "  --json                   the result as JSON\n"
            "Prints method, compile_reason, player, modules (the engine modules the build ships), left_out,\n"
            "module (why one is needed), output, executable, content_manifest, artifact, warning and note\n"
            "lines (and profile: with --profile; device:, installed: and url: with --run-device); exits 1 when the\n"
            "build or the device run fails, 2 on bad arguments.\n",
            executableName, executableName);
    }

}

// ---------------------------------------------------------------- build

int ShippingCommands::runBuild(int argc, char** argv, const char* executableName) {
    attachConsole();
    EditorHostOverrideGuard hostOverride(nullptr);
    fs::path projectPath, out, playersDir;
    std::string platformName, backend, method = "auto", androidFormat, startScene, profileName, runDevice;
    bool noPatch = false;
    double runSeconds = 0.0;
    bool development = false, pack = false, json = false, mismatch = false, simulator = false, allowMissing = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "-h" || arg == "--help") {
            printBuildUsage(executableName);
            return 0;
        } else if (arg == "--project") {
            if (!takeValue(argc, argv, i, value, "build")) return 2;
            projectPath = CommandLine::projectArgPath(value);
        } else if (arg == "--platform") {
            if (!takeValue(argc, argv, i, platformName, "build")) return 2;
        } else if (arg == "--profile") {
            if (!takeValue(argc, argv, i, profileName, "build")) return 2;
        } else if (arg == "--out") {
            if (!takeValue(argc, argv, i, value, "build")) return 2;
            out = CommandLine::absoluteArgPath(value);
        } else if (arg == "--backend") {
            if (!takeValue(argc, argv, i, backend, "build")) return 2;
        } else if (arg == "--method") {
            if (!takeValue(argc, argv, i, method, "build")) return 2;
        } else if (arg == "--android-format") {
            if (!takeValue(argc, argv, i, androidFormat, "build")) return 2;
        } else if (arg == "--start-scene") {
            if (!takeValue(argc, argv, i, startScene, "build")) return 2;
        } else if (arg == "--players-dir") {
            if (!takeValue(argc, argv, i, value, "build")) return 2;
            playersDir = CommandLine::absoluteArgPath(value);
        } else if (arg == "--development") {
            development = true;
        } else if (arg == "--pack") {
            pack = true;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--allow-player-mismatch") {
            mismatch = true;
        } else if (arg == "--ios-simulator") {
            simulator = true;
        } else if (arg == "--allow-missing-assets") {
            allowMissing = true;
        } else if (arg == "--run-device") {
            if (!takeValue(argc, argv, i, runDevice, "build")) return 2;
        } else if (arg == "--no-patch") {
            noPatch = true;
        } else if (arg == "--run-seconds") {
            if (!takeValue(argc, argv, i, value, "build")) return 2;
            char* end = nullptr;
            runSeconds = std::strtod(value.c_str(), &end);
            if (!end || *end != '\0' || runSeconds < 0.0 || runSeconds > 86400.0) {
                std::fprintf(stderr, "build: --run-seconds is 0 to 86400 seconds (got %s)\n", value.c_str());
                return 2;
            }
        } else {
            std::fprintf(stderr, "build: unknown argument %s (try --help)\n", arg.c_str());
            return 2;
        }
    }
    ExportConfig config;
    config.mode = ExportMode::Player;
    if (projectPath.empty() || (profileName.empty() && (platformName.empty() || out.empty()))) {
        std::fprintf(stderr, "build: --project, and --platform and --out (or --profile) are required (try --help)\n");
        return 2;
    }
    if (!platformName.empty() && !parseBuildPlatform(platformName, config.platform)) {
        std::fprintf(stderr, "build: unknown platform %s (windows, linux, macos, android, ios, web)\n", platformName.c_str());
        return 2;
    }
    if (!parseBuildMethod(method, config.method)) {
        std::fprintf(stderr, "build: --method is auto, prebuilt or compile (got %s)\n", method.c_str());
        return 2;
    }
    if (!androidFormat.empty() && !parseAndroidFormat(androidFormat, config.androidFormat)) {
        std::fprintf(stderr, "build: --android-format is aab, apk-per-abi or apk (got %s)\n", androidFormat.c_str());
        return 2;
    }
    if (!backend.empty() && normalizeBackend(backend).empty()) {
        std::fprintf(stderr, "build: unknown backend %s (glcore, gles3, vulkan, d3d11, metal)\n", backend.c_str());
        return 2;
    }
    config.destinationDir = out;
    config.graphicBackend = backend;
    config.developmentBuild = development;
    config.packNativeResources = pack;
    config.playersDir = playersDir;
    config.allowPlayerMismatch = mismatch;
    config.iosSimulator = simulator;
    config.allowMissingAssets = allowMissing;
    config.keystorePassword = envOr("JASPER_ANDROID_KS_PASS");
    config.keyPassword = envOr("JASPER_ANDROID_KEY_PASS");

    if ((noPatch || runSeconds > 0.0) && runDevice.empty()) {
        std::fprintf(stderr, "build: --no-patch and --run-seconds go with --run-device (try --help)\n");
        return 2;
    }

    bool ok = false;
    int exitCode = 1;
    ExportResult result;
    std::string error;
    std::shared_ptr<mcp::builddevice::BuildDeploy> deploy;
    {
        StdoutToStderr quiet;
        Out::setConsoleWindow(nullptr);
        AppSettings::initialize();
        Project project;
        auto platform = std::make_unique<Platform>(&project);
        Engine::systemInit(argc, argv, platform.get());
        EngineRuntimeGuard engineRuntime{project};
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(true);
        ShaderPool::setShaderBuilder([projectPtr = &project](ShaderKey shaderKey) -> ShaderBuildResult {
            static ShaderBuilder builder;
            return builder.buildShader(shaderKey, projectPtr);
        }, &ShaderBuilder::getRunningBuildCount);
        project.setSkipProjectLock(true);
        if (!project.loadProject(projectPath, false)) {
            error = "cannot load the project " + projectPath.string();
        } else {
            config.assetsDir = project.getAssetsDir();
            config.luaDir = project.getLuaDir();
            config.startSceneId = sceneIdOf(project, startScene);
            // no --android-format: the project's (Project Settings > Android > Package Format)
            if (androidFormat.empty()) parseAndroidFormat(project.getAndroidProjectSettings().packageFormat, config.androidFormat);
            // a Build Profile supplies the platform, backend, flavour and folder; an explicit one that differs is refused
            const BuildPlatform asked = config.platform;
            const std::string askedBackend = normalizeBackend(config.graphicBackend);
            bool profileOk = true;
            if (!profileName.empty()) {
                if (!applyBuildProfile(project, profileName, config, error)) {
                    profileOk = false;
                } else if (!platformName.empty() && asked != config.platform) {
                    error = "--platform " + platformName + " differs from Build Profile '" + config.buildProfile + "', which builds for " +
                            buildPlatformName(config.platform);
                    profileOk = false;
                } else if (!backend.empty() && askedBackend != normalizeBackend(config.graphicBackend)) {
                    error = "--backend " + backend + " differs from Build Profile '" + config.buildProfile + "' (" + config.graphicBackend + ")";
                    profileOk = false;
                } else {
                    if (!out.empty()) config.destinationDir = out;
                    if (config.destinationDir.empty()) {
                        error = "--out is needed: Build Profile '" + config.buildProfile + "' has no Output Folder";
                        profileOk = false;
                    }
                }
                if (!profileOk) exitCode = 2;
            }
            if (!profileOk) {
                // the reason is in error, the exit code 2
            } else if (!startScene.empty() && config.startSceneId == 0) {
                error = "--start-scene " + startScene + ": no scene of that id or name";
            } else {
                // --run-device: the device first (its ABI, patch or full install), then the build
                if (!runDevice.empty()) {
                    std::string pickError;
                    const std::vector<platform::RunDevice> devices = platform::devicesOf(config.platform, project.getAndroidProjectSettings().minSdk);
                    const platform::RunDevice* device = platform::pickDevice(devices, config.platform, runDevice, pickError);
                    if (!device) error = "--run-device: " + pickError;
                    else deploy = mcp::builddevice::prepare(&project, config, *device, noPatch ? std::optional<bool>(false) : std::nullopt, error);
                    if (deploy) {
                        config.contentOnly = deploy->patch;
                        config.androidRunAbi = deploy->patch ? std::string() : deploy->androidAbi;
                        if (config.platform == BuildPlatform::IOS) config.iosSimulator = true;
                        if (!deploy->fullBecause.empty() && !deploy->patch && !noPatch) Out::info("Build And Run installs in full: %s", deploy->fullBecause.c_str());
                    }
                }
                if (runDevice.empty() || deploy) {
                    Exporter exporter;
                    ok = exporter.exportProject(&project, config);
                    result = exporter.getResult();
                    if (!ok) error = exporter.getProgress().errorMessage;
                }
                if (ok && deploy) {
                    ok = mcp::builddevice::deploy(*deploy, project.getProjectPath(), result, config, [](const std::string& step) { Out::info("%s", step.c_str()); });
                    if (!ok) error = "the run on " + deploy->device.name + " failed: " + deploy->result.error;
                    // its log until it exits, or for --run-seconds
                    const auto start = std::chrono::steady_clock::now();
                    // (the web page is served by this process: without --run-seconds it serves until interrupted)
                    const bool serving = deploy->device.kind == platform::RunDeviceKind::Browser;
                    if (ok && serving && runSeconds <= 0.0) Out::info("Serving %s until interrupted (Ctrl+C)", deploy->result.url.c_str());
                    while (ok && (runSeconds > 0.0 || serving)) {
                        const std::optional<platform::DeviceRunState> state = platform::DeviceRuns::get().state(deploy->device.id);
                        if (!state || !state->running) break;
                        if (runSeconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= runSeconds) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (ok) {
                        const std::optional<platform::DeviceRunState> state = platform::DeviceRuns::get().state(deploy->device.id);
                        if (state && state->crashed) {
                            ok = false;
                            error = deploy->device.name + " crashed: " + (state->fatalLines.empty() ? std::string("see its log") : state->fatalLines.front());
                        }
                    }
                    // the game keeps running; nothing follows it once the command returns
                    platform::DeviceRuns::get().releaseAll();
                }
            }
        }
    }
    if (json) {
        ordered_json report;
        report["ok"] = ok;
        if (!result.buildProfile.empty()) report["profile"] = result.buildProfile;
        report["method_used"] = buildMethodName(result.methodUsed);
        report["compile_reasons"] = result.compileReasons;
        report["carried"] = result.carried;
        report["player_id"] = result.playerId;
        report["modules"] = result.modules;
        report["modules_left_out"] = result.modulesLeftOut;
        ordered_json evidence = ordered_json::array();
        for (const ModuleEvidence& item : result.moduleEvidence)
            evidence.push_back({{"module", item.module}, {"where", item.where}, {"what", item.what}});
        report["module_evidence"] = evidence;
        report["output"] = result.outputDir.string();
        report["executable"] = result.executable.string();
        report["content_manifest"] = result.contentManifest.string();
        ordered_json artifacts = ordered_json::array();
        for (const fs::path& artifact : result.artifacts) artifacts.push_back(artifact.string());
        report["artifacts"] = artifacts;
        ordered_json warnings = ordered_json::array();
        for (const BuildWarning& warning : result.warnings) warnings.push_back({{"code", warning.code}, {"message", warning.message}});
        report["warnings"] = warnings;
        report["notes"] = result.notes;
        // S53 R21: what ships, by category
        if (!result.buildReport.empty()) {
            report["build_report"] = result.buildReport.string();
            report["build_bytes"] = result.buildBytes;
            // S99 R10, R11: as build_project and build-report.json have them (every category)
            ordered_json categories = ordered_json::object();
            for (const std::string& name : BuildReport::categoryNames())
                categories[name] = result.buildCategories.count(name) ? result.buildCategories.at(name) : 0;
            report["build_categories"] = categories;
            // build-size-tools R35: the history id and the size budget rows the build met
            report["build_report_id"] = result.buildReportId;
            ordered_json budgets = ordered_json::array();
            for (const BuildReportBudget& budget : result.sizeBudgets)
                budgets.push_back({{"platform", budget.platform}, {"category", budget.category}, {"limit_bytes", budget.limitBytes},
                                   {"bytes", budget.bytes}, {"over_bytes", budget.overBytes()}});
            report["size_budgets"] = budgets;
            ordered_json top = ordered_json::array();
            for (size_t i = 0; i < result.largestFiles.size() && i < 5; i++)
                top.push_back({{"path", result.largestFiles[i].path}, {"bytes", result.largestFiles[i].bytes}});
            report["top_files"] = top;
            if (result.downloadBytesGzip) report["download_bytes_gzip"] = result.downloadBytesGzip;
            if (result.downloadBudgetBytes) {
                const uint64_t actual = result.downloadBytesGzip ? result.downloadBytesGzip : result.buildBytes;
                report["download_budget"] = {{"budget_bytes", result.downloadBudgetBytes}, {"actual_bytes", actual},
                                             {"over", actual > result.downloadBudgetBytes}};
            }
            report["stripped_count"] = result.strippedCount;
            report["stripped_bytes"] = result.strippedBytes;
        }
        if (deploy) {
            report["device"] = deploy->device.id;
            report["device_name"] = deploy->device.name;
            report["installed"] = deploy->result.installed;
            report["pushed_files"] = deploy->result.pushedFiles;
            if (!deploy->result.url.empty()) report["url"] = deploy->result.url;
        }
        if (!ok) report["error"] = error;
        std::printf("%s\n", report.dump(2).c_str());
    } else {
        if (!result.buildProfile.empty()) std::printf("profile: %s\n", result.buildProfile.c_str());
        if (result.methodUsed != BuildMethod::Auto) std::printf("method: %s\n", buildMethodName(result.methodUsed));
        for (const std::string& reason : result.compileReasons) std::printf("compile_reason: %s\n", reason.c_str());
        for (const std::string& item : result.carried) std::printf("carried: %s\n", item.c_str());
        if (ok) {
            if (!result.playerId.empty()) std::printf("player: %s\n", result.playerId.c_str());
            auto joined = [](const std::vector<std::string>& names) {
                std::string text;
                for (const std::string& name : names) text += (text.empty() ? "" : " ") + name;
                return text.empty() ? std::string("none") : text;
            };
            std::printf("modules: %s\n", joined(result.modules).c_str());
            std::printf("left_out: %s\n", joined(result.modulesLeftOut).c_str());
            for (const std::string& module : result.modules) {
                for (const ModuleEvidence& item : result.moduleEvidence) {
                    if (item.module != module) continue;
                    std::printf("module: %s: %s: %s\n", module.c_str(), item.where.c_str(), item.what.c_str());
                    break;
                }
            }
            std::printf("output: %s\n", result.outputDir.string().c_str());
            if (!result.executable.empty()) std::printf("executable: %s\n", result.executable.string().c_str());
            if (!result.contentManifest.empty()) std::printf("content_manifest: %s\n", result.contentManifest.string().c_str());
            for (const fs::path& artifact : result.artifacts) std::printf("artifact: %s\n", artifact.string().c_str());
        }
        for (const BuildWarning& warning : result.warnings) std::printf("warning: %s: %s\n", warning.code.c_str(), warning.message.c_str());
        for (const std::string& note : result.notes) std::printf("note: %s\n", note.c_str());
        if (deploy && !deploy->result.installed.empty()) {
            std::printf("device: %s (%s)\n", deploy->device.id.c_str(), deploy->device.name.c_str());
            std::printf("installed: %s\n", deploy->result.installed.c_str());
            if (deploy->result.installed == "patched") std::printf("pushed_files: %zu\n", deploy->result.pushedFiles);
            if (!deploy->result.url.empty()) std::printf("url: %s\n", deploy->result.url.c_str());
        }
    }
    std::fflush(stdout);
    if (!ok) {
        std::fprintf(stderr, "build: %s\n", error.empty() ? "the build failed" : error.c_str());
        return exitCode;
    }
    return 0;
}

// ---------------------------------------------------------------- players

int ShippingCommands::runPlayers(int argc, char** argv, const char* executableName) {
    attachConsole();
    std::string action;
    std::vector<std::string> operands;
    fs::path playersDir;
    bool json = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "-h" || arg == "--help") {
            std::printf("Usage: %s players list | install <archive.zip> | download <id> | remove <id> [--players-dir D] [--json]\n"
                        "The prebuilt player runtimes builds use (one per <platform>-<arch>-<backend>-<flavour>).\n",
                        executableName);
            return 0;
        } else if (arg == "--players-dir") {
            if (!takeValue(argc, argv, i, value, "players")) return 2;
            playersDir = CommandLine::absoluteArgPath(value);
        } else if (arg == "--json") {
            json = true;
        } else if (action.empty()) {
            action = arg;
        } else {
            operands.push_back(arg);
        }
    }
    if (action == "list") {
        const std::vector<PlayerInfo> players = PlayerRuntimes::list(playersDir);
        if (json) {
            ordered_json list = ordered_json::array();
            for (const PlayerInfo& p : players)
                list.push_back({{"id", p.id}, {"engine_version", p.engineVersion}, {"dir", p.dir.string()}, {"size_mb", double(p.sizeBytes) / (1024.0 * 1024.0)}});
            std::printf("%s\n", ordered_json{{"editor_version", PlayerRuntimes::editorVersion()}, {"install_root", PlayerRuntimes::installRoot(playersDir).string()},
                                              {"players", list}}.dump(2).c_str());
        } else {
            std::printf("editor_version: %s\ninstall_root: %s\n", PlayerRuntimes::editorVersion().c_str(), PlayerRuntimes::installRoot(playersDir).string().c_str());
            for (const PlayerInfo& p : players)
                std::printf("player: %s %s %.1f MB %s\n", p.id.c_str(), p.engineVersion.c_str(), double(p.sizeBytes) / (1024.0 * 1024.0), p.dir.string().c_str());
        }
        return 0;
    }
    if ((action == "install" || action == "download" || action == "remove") && operands.size() != 1) {
        std::fprintf(stderr, "players %s: needs one %s\n", action.c_str(), action == "install" ? "archive" : "runtime id");
        return 2;
    }
    std::string installed, error;
    std::vector<std::string> warnings;
    bool ok = false;
    if (action == "install") {
        ok = PlayerRuntimes::install(CommandLine::absoluteArgPath(operands[0]), playersDir, installed, warnings, error);
    } else if (action == "download") {
        ok = PlayerRuntimes::download(operands[0], playersDir, nullptr, installed, warnings, error);
    } else if (action == "remove") {
        ok = PlayerRuntimes::remove(operands[0], playersDir, error);
        installed = operands[0];
    } else {
        std::fprintf(stderr, "players: list, install, download or remove (try --help)\n");
        return 2;
    }
    for (const std::string& warning : warnings) std::printf("warning: %s\n", warning.c_str());
    if (!ok) {
        std::fprintf(stderr, "players %s: %s\n", action.c_str(), error.c_str());
        return 1;
    }
    std::printf("%s: %s\n", action == "remove" ? "removed" : "installed", installed.c_str());
    return 0;
}

// ---------------------------------------------------------------- publish

int ShippingCommands::runPublish(int argc, char** argv, const char* executableName) {
    attachConsole();
    std::string action, release, branch, user = envOr("STEAM_BUILD_USER"), executable;
    std::vector<std::string> targets, operands;
    fs::path projectDir = CommandLine::invocationDirectory();
    bool preview = false, force = false, json = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "-h" || arg == "--help") {
            std::printf(
                "Usage: %s publish <action> [--project P] ...\n"
                "  init [--force]                          write publishing.json (the targets, Steam, the Deck)\n"
                "  prepare --release R [--target t]...      validate, copy, split symbols, release.json\n"
                "  verify <release>                        the checks again; nothing changed since prepare\n"
                "  steam-vdf <release> [--preview] [--branch b]   the SteamPipe scripts\n"
                "  steam-upload <release> [--user u] [--branch b] steamcmd +run_app_build (STEAM_BUILD_USER)\n"
                "  deck-deploy <release> --target t        copy to the Deck over ssh, keep one previous build\n"
                "  deck-rollback                           restore the previous build on the Deck\n"
                "  deck-smoke [--executable e]             run it there (--quit-after 600 --fail-on-error)\n"
                "A <release> is a name under the releases directory or a path.\n",
                executableName);
            return 0;
        } else if (arg == "--project") {
            if (!takeValue(argc, argv, i, value, "publish")) return 2;
            projectDir = CommandLine::projectArgPath(value);
        } else if (arg == "--release") {
            if (!takeValue(argc, argv, i, release, "publish")) return 2;
        } else if (arg == "--target") {
            if (!takeValue(argc, argv, i, value, "publish")) return 2;
            targets.push_back(value);
        } else if (arg == "--branch") {
            if (!takeValue(argc, argv, i, branch, "publish")) return 2;
        } else if (arg == "--user") {
            if (!takeValue(argc, argv, i, user, "publish")) return 2;
        } else if (arg == "--executable") {
            if (!takeValue(argc, argv, i, executable, "publish")) return 2;
        } else if (arg == "--preview") {
            preview = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--json") {
            json = true;
        } else if (action.empty()) {
            action = arg;
        } else {
            operands.push_back(arg);
        }
    }
    const fs::path configPath = publish::PublishingConfig::pathOf(projectDir);
    std::string error;
    publish::PublishTools tools;
    tools.log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };

    if (action == "init") {
        std::error_code ec;
        if (fs::exists(configPath, ec) && !force) {
            std::fprintf(stderr, "publish init: %s exists (--force replaces it)\n", configPath.string().c_str());
            return 1;
        }
        const publish::PublishingConfig config = publish::defaultPublishingConfig({{"linux", "build/linux"}});
        if (!config.save(configPath, error)) {
            std::fprintf(stderr, "publish init: %s\n", error.c_str());
            return 1;
        }
        std::printf("publishing: %s\n", configPath.string().c_str());
        return 0;
    }

    publish::PublishingConfig config;
    const bool needsConfig = action == "prepare" || action == "deck-deploy" || action == "deck-rollback" || action == "deck-smoke";
    if (!publish::PublishingConfig::load(configPath, config, error)) {
        if (needsConfig) {
            std::fprintf(stderr, "publish %s: %s\n", action.c_str(), error.c_str());
            return 1;
        }
    }
    // a release name under the releases directory, or a path
    auto releaseDir = [&](const std::string& value) {
        std::error_code ec;
        const fs::path asPath = CommandLine::absoluteArgPath(value);
        if (fs::is_regular_file(asPath / "release.json", ec)) return asPath;
        const fs::path root = editor::FileUtils::pathFromUtf8(config.releasesDir).is_absolute() ? editor::FileUtils::pathFromUtf8(config.releasesDir) : projectDir / editor::FileUtils::pathFromUtf8(config.releasesDir);
        return root / value;
    };
    const std::string releaseArg = !operands.empty() ? operands.front() : release;

    if (action == "prepare") {
        if (release.empty() && !operands.empty()) release = operands.front();
        publish::PrepareRequest request;
        request.projectDir = projectDir;
        request.projectName = projectDir.filename().string();
        request.config = config;
        request.release = release;
        request.targets = targets;
        publish::PrepareResult result;
        std::vector<std::string> problems;
        const bool ok = publish::prepareRelease(request, tools, result, problems);
        for (const std::string& warning : result.warnings) std::printf("warning: %s\n", warning.c_str());
        if (!ok) {
            for (const std::string& problem : problems) std::fprintf(stderr, "publish prepare: %s\n", problem.c_str());
            return 1;
        }
        std::printf("release: %s\n", result.releaseDir.string().c_str());
        return 0;
    }
    if (action == "verify") {
        if (releaseArg.empty()) {
            std::fprintf(stderr, "publish verify: which release?\n");
            return 2;
        }
        std::vector<std::string> problems;
        publish::ReleaseInfo info;
        const bool ok = publish::verifyRelease(releaseDir(releaseArg), problems, &info);
        if (json) {
            ordered_json report = {{"ok", ok}, {"release", info.release}, {"problems", problems}};
            ordered_json list = ordered_json::object();
            for (const auto& [name, target] : info.targets)
                list[name] = {{"platform", target.platform}, {"arch", target.arch}, {"player_id", target.playerId}, {"files", target.files.size()},
                              {"glibc_floor", target.glibcFloor}};
            report["targets"] = list;
            std::printf("%s\n", report.dump(2).c_str());
        } else {
            for (const auto& [name, target] : info.targets) {
                std::printf("target: %s %s-%s %zu files\n", name.c_str(), target.platform.c_str(), target.arch.c_str(), target.files.size());
                if (!target.glibcFloor.empty()) std::printf("glibc_floor: %s GLIBC_%s\n", name.c_str(), target.glibcFloor.c_str());
            }
            for (const std::string& problem : problems) std::fprintf(stderr, "publish verify: %s\n", problem.c_str());
            if (ok) std::printf("verified: %s\n", info.release.c_str());
        }
        return ok ? 0 : 1;
    }
    if (action == "steam-vdf") {
        fs::path app;
        std::vector<fs::path> depots;
        if (releaseArg.empty() || !publish::writeSteamScripts(releaseDir(releaseArg), preview, branch, app, depots, error)) {
            std::fprintf(stderr, "publish steam-vdf: %s\n", releaseArg.empty() ? "which release?" : error.c_str());
            return releaseArg.empty() ? 2 : 1;
        }
        std::printf("app_build: %s\n", app.string().c_str());
        for (const fs::path& depot : depots) std::printf("depot_build: %s\n", depot.string().c_str());
        return 0;
    }
    if (action == "steam-upload") {
        if (releaseArg.empty()) {
            std::fprintf(stderr, "publish steam-upload: which release?\n");
            return 2;
        }
        const platform::ToolLocation steamcmd = platform::ExternalTools::steamcmd();
        if (!publish::steamUpload(releaseDir(releaseArg), user, branch, steamcmd.path, tools, error)) {
            std::fprintf(stderr, "publish steam-upload: %s\n", error.c_str());
            return 1;
        }
        std::printf("uploaded: %s\n", releaseArg.c_str());
        return 0;
    }
    if (action == "deck-deploy" || action == "deck-rollback" || action == "deck-smoke") {
        std::string output;
        bool ok = false;
        int code = 0;
        if (action == "deck-deploy") {
            std::string target = targets.empty() ? "" : targets.front();
            if (target.empty())
                for (const auto& [name, t] : config.targets)
                    if (t.deviceProfile == "steamdeck") target = name;
            if (releaseArg.empty() || target.empty()) {
                std::fprintf(stderr, "publish deck-deploy: <release> --target <t> (a linux x64 target)\n");
                return 2;
            }
            ok = publish::deckDeploy(releaseDir(releaseArg), target, config.deck, tools, output, error);
        } else if (action == "deck-rollback") {
            ok = publish::deckRollback(config.deck, tools, output, error);
        } else {
            if (executable.empty())
                for (const auto& [name, t] : config.targets)
                    if (t.deviceProfile == "steamdeck" || (executable.empty() && t.platform == "linux")) executable = t.executable;
            ok = publish::deckSmoke(config.deck, executable, tools, code, output, error);
        }
        std::printf("%s", output.c_str());
        if (!ok) {
            std::fprintf(stderr, "publish %s: %s\n", action.c_str(), error.c_str());
            return 1;
        }
        if (action == "deck-smoke") {
            std::printf("exit_code: %d\n", code);
            return code == 0 ? 0 : 1;
        }
        return 0;
    }
    std::fprintf(stderr, "publish: unknown action '%s' (try --help)\n", action.c_str());
    return 2;
}

int ShippingCommands::runReflect(int argc, char** argv, const char* executableName) {
    std::string output;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--emit-runtime-fields" && i + 1 < argc) {
            output = CommandLine::absoluteArgPath(argv[++i]).string();
        } else if (arg == "-h" || arg == "--help") {
            std::printf("Usage: %s reflect --emit-runtime-fields <file>\n"
                        "Writes the cooked scenes' runtime field tables (engine/core/serialization/RuntimeFields.gen.inc)\n"
                        "from the editor's reflection tables.\n", executableName);
            return 0;
        } else {
            std::fprintf(stderr, "reflect: unknown argument %s (try --help)\n", arg.c_str());
            return 2;
        }
    }
    if (output.empty()) {
        std::fprintf(stderr, "reflect: --emit-runtime-fields <file> is required\n");
        return 2;
    }
    std::vector<std::string> errors;
    const std::string text = cook::emitRuntimeFields(errors);
    for (const std::string& error : errors) std::fprintf(stderr, "reflect: %s\n", error.c_str());
    if (!errors.empty()) return 1;
    const io::WriteResult written = io::writeFileAtomic(output, text);
    if (!written.ok()) {
        std::fprintf(stderr, "reflect: %s\n", io::describeWriteError(written).c_str());
        return 1;
    }
    std::printf("runtime_fields: %s\nbytes: %zu\n", output.c_str(), text.size());
    return 0;
}
