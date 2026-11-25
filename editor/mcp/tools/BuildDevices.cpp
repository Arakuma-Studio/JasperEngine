// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildDevices.h"

#include "util/CppIdentifier.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "mcp/McpContext.h"
#include "mcp/McpServer.h"
#include "platform/AndroidPackager.h"
#include "players/PlayerRuntimes.h"
#include "thread/ThreadUtil.h"
#include "util/Base64.h"
#include "util/BuiltPlayers.h"
#include "util/FileUtils.h"
#include "util/ZipArchive.h"
#include "io/AtomicFile.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    // the mode BuiltPlayers records a platform's Build And Run package under
    std::string packageMode(BuildPlatform platform) {
        return std::string("player-") + buildPlatformName(platform);
    }

    // the build cache's cooked content of a platform (Exporter::runPrebuilt's)
    fs::path cacheContent(const fs::path& internalDir, BuildPlatform platform) {
        return internalDir / "export" / ("player-" + std::string(buildPlatformName(platform))) / "content";
    }

    bool runHeadless() {
        return editor::Platform::isHeadless();
    }

    // the device's first ABI the project builds (R3), "" with the error naming both lists
    std::string chooseAbi(const RunDevice& device, const AndroidProjectSettings& android, std::string& error) {
        const std::vector<std::string> selected = selectedAndroidAbis(android);
        for (const std::string& abi : device.abis)
            if (std::find(selected.begin(), selected.end(), abi) != selected.end()) return abi;
        auto join = [](const std::vector<std::string>& list) {
            std::string text;
            for (const std::string& item : list) text += (text.empty() ? "" : ", ") + item;
            return text.empty() ? std::string("none") : text;
        };
        error = device.name + " runs " + join(device.abis) + "; the project builds " + join(selected) +
                " (Project Settings > Android > Architectures): select one the device runs";
        return "";
    }

    // the platforms a run_device / device value may name
    std::vector<BuildPlatform> devicePlatforms() {
        std::vector<BuildPlatform> platforms = {BuildPlatform::Android, BuildPlatform::IOS, BuildPlatform::Web};
        if (std::find(platforms.begin(), platforms.end(), hostBuildPlatform()) == platforms.end()) platforms.push_back(hostBuildPlatform());
        return platforms;
    }

    Json deviceJson(const RunDevice& device) {
        Json item = {{"id", device.id},
                     {"platform", buildPlatformName(device.platform)},
                     {"kind", runDeviceKindName(device.kind)},
                     {"name", device.name},
                     {"state", device.state},
                     {"usable", device.usable}};
        if (!device.why.empty()) item["why"] = device.why;
        if (!device.abis.empty()) item["abi"] = device.abis.front();
        if (!device.osVersion.empty()) item["os_version"] = device.osVersion;
        return item;
    }

    // the identity of the full development player of a platform (what a patch must match)
    struct PlayerIdentity {
        std::string id, identity;
        bool debuggable = false;
        bool found = false;
    };
    PlayerIdentity identityOf(const ExportConfig& config, bool development) {
        PlayerIdentity out;
        out.id = Exporter::playerIdFor(config, development);
        std::string error;
        if (const std::optional<PlayerInfo> info = PlayerRuntimes::find(out.id, config.playersDir, config.allowPlayerMismatch, error)) {
            out.found = true;
            out.identity = playerIdentityOf(info->executables);
            out.debuggable = config.platform != BuildPlatform::Android || androidRuntimeHasDebuggableMarker(info->dir / "android");
        }
        return out;
    }

    // what a full install from run_build records as the device's content: the package's own manifest
    fs::path installedContentOf(const fs::path& package, BuildPlatform platform, const fs::path& internalDir) {
        if (platform == BuildPlatform::IOS) return package / "Content";
        if (platform != BuildPlatform::Android) return {};
        ZipReader zip;
        std::string error;
        std::vector<uint8_t> bytes;
        if (!zip.open(package, error) || !zip.read("assets/game/content-manifest.json", bytes, error)) return {};
        const fs::path dir = internalDir / "export" / "player-android" / "installed-content";
        safewrite::EditorInternal own(dir);
        if (!io::writeFileAtomic(dir / "content-manifest.json", std::string(bytes.begin(), bytes.end())).ok()) return {};
        return dir;
    }

    bool deviceReadText(const fs::path& path, std::string& text) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    bool sameContent(const std::map<std::string, player::ManifestFile>& a, const std::map<std::string, player::ManifestFile>& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                   return x.first == y.first && x.second.sha256 == y.second.sha256 && x.second.size == y.second.size;
               });
    }

    Json imageEntry(ToolResult& result, std::vector<uint8_t>& png, double at, const std::string& from) {
        Json item = {{"at_seconds", std::round(at * 100.0) / 100.0}, {"from", from}, {"image", static_cast<int>(result.images.size())}};
        result.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
        return item;
    }

}

std::shared_ptr<builddevice::BuildDeploy> builddevice::prepare(Project* project, const ExportConfig& config, const RunDevice& device,
                                                                std::optional<bool> patch, std::string& error) {
    const BuildPlatform platform = config.platform;
    const bool development = config.developmentBuild || project->isDevelopmentBuild();
    auto deploy = std::make_shared<BuildDeploy>();
    deploy->device = device;
    deploy->headless = runHeadless();
    if (platform == BuildPlatform::Android) {
        deploy->androidAbi = chooseAbi(device, project->getAndroidProjectSettings(), error);
        if (deploy->androidAbi.empty()) return nullptr;
    }
    const PlayerIdentity identity = identityOf(config, development);
    deploy->playerId = identity.id;
    deploy->playerIdentity = identity.identity;
    const bool wantPatch = patch.value_or(development);
    if (wantPatch && (platform == BuildPlatform::Android || platform == BuildPlatform::IOS)) {
        const DeviceRecords records = DeviceRecords::load(project->getProjectInternalPath());
        const PatchDecision decision = decidePatch(records.find(device.id), device, development, identity.id, identity.identity, identity.debuggable);
        deploy->patch = decision.patch && identity.found;
        deploy->fullBecause = identity.found ? decision.why : "no " + identity.id + " player runtime is installed";
    }
    return deploy;
}

void builddevice::resolve(const CallPtr& call, Project* project, const ExportConfig& config, const std::string& runDevice, std::optional<bool> patch,
                          std::function<void(std::shared_ptr<BuildDeploy>)> then) {
    const BuildPlatform platform = config.platform;
    if (runDevice == "web" && platform != BuildPlatform::Web) {
        call->fail("run_device web runs a web build: build with platform web (or mode web).");
        return;
    }
    if (platform != BuildPlatform::Android && platform != BuildPlatform::IOS && platform != BuildPlatform::Web && platform != hostBuildPlatform()) {
        call->fail(std::string("run_device: a ") + buildPlatformName(platform) + " game does not run on this computer; build for " +
                   buildPlatformName(hostBuildPlatform()) + ", android, ios or web.");
        return;
    }
    const unsigned int minSdk = project->getAndroidProjectSettings().minSdk;
    struct Probe {
        std::atomic<bool> done{false};
        std::vector<RunDevice> devices;
    };
    auto probe = std::make_shared<Probe>();
    jasperengine::thread::spawnNamed("je-device-probe", [probe, platform, minSdk]() {
        probe->devices = devicesOf(platform, minSdk);
        probe->done = true;
    }).detach();
    call->waitUntil([probe]() { return probe->done.load(); }, 30000, [=](bool timedOut) {
        if (timedOut) {
            call->fail("The device probe did not answer in 30 s: restart adb (adb kill-server) and try again.");
            return;
        }
        std::string error;
        const RunDevice* device = pickDevice(probe->devices, platform, runDevice, error);
        if (!device) {
            call->fail("run_device: " + error + ".");
            return;
        }
        std::shared_ptr<BuildDeploy> deploy = prepare(project, config, *device, patch, error);
        if (!deploy) {
            call->fail("run_device: " + error + ".");
            return;
        }
        then(deploy);
    });
}

bool builddevice::deploy(BuildDeploy& deploy, const fs::path& projectPath, const ExportResult& result, const ExportConfig& config,
                         const std::function<void(const std::string&)>& progress) {
    const fs::path internalDir = projectPath / ".jasperengine";
    DeployRequest request;
    request.internalDir = internalDir;
    request.playerId = deploy.playerId;
    request.playerIdentity = deploy.playerIdentity;
    request.debuggable = result.debuggable;
    request.run.device = deploy.device;
    request.run.patch = deploy.patch;
    request.run.appId = result.appId;
    request.run.contentDir = result.contentDir;
    // (a Development Build of the project's setting too: the player id says it)
    request.run.development = config.developmentBuild || deploy.playerId.find("-development") != std::string::npos;
    request.run.headless = deploy.headless;
    if (!deploy.patch) {
        fs::path package = result.executable;
        if (config.platform == BuildPlatform::IOS) {
            // the simulator .app among the artifacts
            for (const fs::path& artifact : result.artifacts)
                if (artifact.extension() == ".app") package = artifact;
        }
        request.run.package = package;
        request.run.executableName = package.stem().string();
        // R15: the package the editor built, recorded now and verified as run_build verifies
        if (!builtplayers::record(internalDir, package, packageMode(config.platform))) {
            deploy.result.error = "cannot record the built package " + FileUtils::pathToGenericUtf8(package);
            return false;
        }
        const std::string why = builtplayers::verify(internalDir, package);
        if (!why.empty()) {
            deploy.result.error = FileUtils::pathToGenericUtf8(package) + ": " + why;
            return false;
        }
    }
    deploy.result = DeviceRuns::get().deploy(request, progress);
    return deploy.result.ok;
}

Json builddevice::resultJson(const BuildDeploy& deploy) {
    Json device = {{"id", deploy.device.id},
                   {"name", deploy.device.name},
                   {"installed", deploy.result.installed},
                   {"pushed_files", deploy.result.pushedFiles},
                   {"pushed_bytes", deploy.result.pushedBytes},
                   {"launch_ms", std::round(deploy.result.launchMs)}};
    if (!deploy.result.url.empty()) device["url"] = deploy.result.url;
    return device;
}

std::string builddevice::nextHint(const BuildDeploy& deploy) {
    std::string next = "read_console categories [\"device\"] for its log; run_build device to watch it";
    if (!deploy.fullBecause.empty() && !deploy.patch) next = "installed in full: " + deploy.fullBecause + ". " + next;
    return next;
}

void builddevice::replyWithDevices(const CallPtr& call, Project* project, const std::string& text, Json data) {
    const unsigned int minSdk = project->getAndroidProjectSettings().minSdk;
    const fs::path internalDir = project->getProjectInternalPath();
    const bool development = project->isDevelopmentBuild();
    struct Probe {
        std::atomic<bool> done{false};
        std::vector<RunDevice> devices;
    };
    auto probe = std::make_shared<Probe>();
    jasperengine::thread::spawnNamed("je-device-probe", [probe, minSdk]() {
        for (BuildPlatform platform : devicePlatforms()) {
            const std::vector<RunDevice> found = devicesOf(platform, minSdk);
            probe->devices.insert(probe->devices.end(), found.begin(), found.end());
        }
        probe->done = true;
    }).detach();
    call->waitUntil([probe]() { return probe->done.load(); }, 30000, [=](bool timedOut) mutable {
        if (timedOut) {
            call->fail("The device probe did not answer in 30 s: restart adb (adb kill-server) and try again.");
            return;
        }
        const DeviceRecords records = DeviceRecords::load(internalDir);
        Json devices = Json::array();
        size_t usable = 0;
        for (const RunDevice& device : probe->devices) {
            Json item = deviceJson(device);
            if (device.usable) usable++;
            if (const DeviceRecord* record = records.find(device.id)) {
                ExportConfig config;
                config.platform = device.platform;
                const PlayerIdentity identity = identityOf(config, development);
                const PatchDecision decision = decidePatch(record, device, development, identity.id, identity.identity, identity.debuggable);
                Json installed = {{"package", record->package}, {"patch", record->patch}, {"patchable", decision.patch}};
                if (!decision.patch) installed["why_not"] = decision.why;
                item["installed"] = installed;
            }
            devices.push_back(item);
        }
        data["devices"] = devices;
        call->ok(text + " " + std::to_string(usable) + " usable Run Device(s) of " + std::to_string(devices.size()) + ".", data);
    });
}

// ------------------------------------------------------------------ run_build device

void builddevice::runBuildOnDevice(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    for (const char* key : {"input", "space", "gap_ms", "input_delay_seconds"}) {
        if (args.contains(key) && !args[key].is_null()) {
            call->fail("input is desktop-only (Linux/X11); on a device drive the game from Lua or its arguments.");
            return;
        }
    }
    const std::string deviceId = args["device"].get<std::string>();
    const fs::path internalDir = project->getProjectInternalPath();
    const unsigned int minSdk = project->getAndroidProjectSettings().minSdk;
    fs::path path;
    if (args.contains("path") && args["path"].is_string() && !args["path"].get<std::string>().empty()) {
        path = FileUtils::pathFromUtf8(args["path"].get<std::string>());
        if (!path.is_absolute()) {
            call->fail("path must be absolute: the package this project built (.apk, simulator .app, web build folder).");
            return;
        }
    }
    // the player's options (R4)
    std::vector<std::string> playerArgs;
    if (args.contains("fixed_fps") && args["fixed_fps"].is_number()) playerArgs.insert(playerArgs.end(), {"--fixed-fps", std::to_string(args["fixed_fps"].get<double>())});
    if (args.contains("quit_after") && args["quit_after"].is_number_integer())
        playerArgs.insert(playerArgs.end(), {"--quit-after", std::to_string(args["quit_after"].get<long long>())});
    if (args.value("after_ready", false)) playerArgs.push_back("--after-ready");
    if (args.contains("args") && args["args"].is_array()) {
        for (const Json& a : args["args"]) {
            if (!a.is_string()) {
                call->fail("args must be strings.");
                return;
            }
            playerArgs.push_back(a.get<std::string>());
        }
    }
    std::vector<double> shotsAt;
    if (args.contains("screenshots_at") && args["screenshots_at"].is_array())
        for (const Json& at : args["screenshots_at"])
            if (at.is_number()) shotsAt.push_back(std::clamp(at.get<double>(), 0.1, 300.0));
    const double seconds = std::clamp(args.value("seconds", 5.0), 0.5, 300.0);
    const bool wantShot = args.value("screenshot", shotsAt.empty());
    if (wantShot && shotsAt.empty()) shotsAt.push_back(std::clamp(args.value("screenshot_after_seconds", std::min(3.0, seconds * 0.8)), 0.1, seconds));
    std::sort(shotsAt.begin(), shotsAt.end());
    const bool keepRunning = args.value("keep_running", false);
    const bool quits = args.contains("quit_after");
    const int maxLines = std::clamp(args.value("log_lines", 80), 0, 2000);
    const bool headless = runHeadless();
    const std::string app = project->getName().empty() ? "jasperengine-project" : toIdentifier(project->getName());
    const std::map<BuildPlatform, std::string> appIds = {{BuildPlatform::Android, Exporter::applicationIdOf(*project, BuildPlatform::Android, app)},
                                                         {BuildPlatform::IOS, Exporter::applicationIdOf(*project, BuildPlatform::IOS, app)}};

    struct Outcome {
        std::atomic<bool> done{false};
        ToolResult result;
    };
    auto outcome = std::make_shared<Outcome>();
    jasperengine::thread::spawnNamed("je-run-device", [=]() {
        ToolResult& result = outcome->result;
        auto failWith = [&](const std::string& message) {
            result = ToolResult::error(message);
            outcome->done = true;
        };
        // the platform: the package's kind, else where the device is
        BuildPlatform platform = BuildPlatform::Android;
        std::vector<RunDevice> devices;
        std::error_code ec;
        if (deviceId == "web" || (!path.empty() && (fs::is_directory(path, ec) ? fs::exists(path / "index.html", ec) && path.extension() != ".app"
                                                                                : path.extension() == ".html"))) {
            platform = BuildPlatform::Web;
        } else if (!path.empty() && path.extension() == ".app") {
            platform = BuildPlatform::IOS;
        } else if (path.empty() || path.extension() != ".apk") {
            devices = devicesOf(BuildPlatform::Android, minSdk);
            const bool android = std::any_of(devices.begin(), devices.end(), [&](const RunDevice& d) {
                return d.id == deviceId || (deviceId == "auto" && d.usable);
            });
            if (!android && path.empty()) platform = BuildPlatform::IOS;
        }
        if (devices.empty() || platform != BuildPlatform::Android) devices = devicesOf(platform, minSdk);
        std::string error;
        const RunDevice* picked = pickDevice(devices, platform, deviceId, error);
        if (!picked) {
            failWith("device: " + error + ".");
            return;
        }
        const RunDevice device = *picked;
        // R15: only the package this project built, unchanged
        fs::path package = path;
        const fs::path recorded = builtplayers::recordedPath(internalDir, packageMode(platform));
        if (package.empty()) package = recorded;
        if (platform == BuildPlatform::Web && fs::is_directory(package, ec)) package /= "index.html";
        if (package.empty()) {
            failWith(std::string("No ") + buildPlatformName(platform) + " build is recorded: build_project platform " + buildPlatformName(platform) +
                     " run_device \"" + deviceId + "\" builds and runs it.");
            return;
        }
        const std::string why = builtplayers::verify(internalDir, package);
        if (!why.empty() || (!recorded.empty() && safewrite::resolvedPath(package) != safewrite::resolvedPath(recorded))) {
            failWith("run_build device installs only the package this project built (build_project run_device records it). " +
                     FileUtils::pathToGenericUtf8(package) + ": " + (why.empty() ? "it is not the last package built for " + std::string(buildPlatformName(platform)) : why) + ".");
            return;
        }

        DeployRequest request;
        request.internalDir = internalDir;
        request.run.device = device;
        request.run.package = package;
        request.run.playerArgs = playerArgs;
        request.run.headless = headless;
        request.run.executableName = package.stem().string();
        const DeviceRecords records = DeviceRecords::load(internalDir);
        const DeviceRecord* record = records.find(device.id);
        bool launchOnly = false;
        if (platform == BuildPlatform::Android || platform == BuildPlatform::IOS) {
            const fs::path cache = cacheContent(internalDir, platform);
            player::ContentManifest cached;
            std::string cacheText, parseError;
            const bool haveCache = deviceReadText(cache / "content-manifest.json", cacheText) &&
                                   player::ContentManifest::parse(cacheText, cached, parseError);
            std::string sha;
            const builtplayers::Fingerprint print = builtplayers::fingerprintOf(package);
            sha = platform == BuildPlatform::Android ? print.sha256 : std::string();
            const bool sameInstall = record && (platform != BuildPlatform::Android || record->packageSha256 == sha);
            if (sameInstall) {
                request.run.appId = record->package;
                request.playerId = record->playerId;
                request.playerIdentity = record->playerSha256;
                request.debuggable = record->debuggable;
                request.run.development = record->development;
                // the device runs other content than the last cook: a patch brings it
                if (haveCache && !sameContent(record->content, cached.files) && record->development &&
                    (platform != BuildPlatform::Android || record->debuggable)) {
                    request.run.patch = true;
                    request.run.contentDir = cache;
                } else {
                    launchOnly = true;
                }
            } else {
                // a full install: the package's own content is what the device then runs
                request.run.contentDir = installedContentOf(package, platform, internalDir);
                if (request.run.contentDir.empty()) {
                    failWith("cannot read the content manifest inside " + FileUtils::pathToGenericUtf8(package) + ": build it again.");
                    return;
                }
                player::ContentManifest inside;
                std::string text;
                if (deviceReadText(request.run.contentDir / "content-manifest.json", text) && player::ContentManifest::parse(text, inside, parseError)) {
                    request.playerId = inside.playerId;
                    ExportConfig config;
                    config.platform = platform;
                    config.graphicBackend = inside.backend;
                    const bool development = inside.playerId.find("-development") != std::string::npos;
                    const PlayerIdentity identity = identityOf(config, development);
                    request.playerIdentity = identity.identity;
                    request.run.development = development;
                    request.debuggable = development && identity.debuggable;
                }
                request.run.appId = record && !record->package.empty() ? record->package : appIds.at(platform);
            }
        }

        const auto started = std::chrono::steady_clock::now();
        // the device runs this very build: launched only
        request.launchOnly = launchOnly;
        const DeployResult deployed = DeviceRuns::get().deploy(request);
        if (!deployed.ok) {
            failWith(deployed.error + ".");
            return;
        }
        auto elapsed = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
        Json shots = Json::array();
        size_t nextShot = 0;
        while (true) {
            const std::optional<DeviceRunState> state = DeviceRuns::get().state(device.id);
            const double t = elapsed();
            if (nextShot < shotsAt.size() && t >= shotsAt[nextShot] && state && state->running) {
                std::vector<uint8_t> png;
                std::string shotError;
                if (DeviceRuns::get().screenshot(device.id, png, shotError)) shots.push_back(imageEntry(result, png, t, "device"));
                else shots.push_back({{"at_seconds", std::round(t * 100.0) / 100.0}, {"error", shotError}});
                nextShot++;
                continue;
            }
            if (!state || !state->running) break;
            if (t >= seconds && (!quits || t >= 300.0)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (; nextShot < shotsAt.size(); nextShot++) shots.push_back({{"at_seconds", shotsAt[nextShot]}, {"error", "not taken: the app exited first"}});
        std::optional<DeviceRunState> state = DeviceRuns::get().state(device.id);
        const bool stoppedHere = !keepRunning && state && state->running;
        if (stoppedHere) DeviceRuns::get().stop(device.id);
        state = DeviceRuns::get().state(device.id);
        const DeviceRunState s = state.value_or(DeviceRunState());

        Json tail = Json::array();
        for (size_t i = s.logTail.size() > static_cast<size_t>(maxLines) ? s.logTail.size() - maxLines : 0; i < s.logTail.size(); i++)
            tail.push_back(s.logTail[i]);
        Json data = {{"device", {{"id", device.id}, {"name", device.name}, {"installed", launchOnly ? "current" : deployed.installed},
                                 {"pushed_files", deployed.pushedFiles}, {"pushed_bytes", deployed.pushedBytes},
                                 {"launch_ms", std::round(deployed.launchMs)}}},
                     {"ran_seconds", std::round(elapsed() * 10.0) / 10.0},
                     {"log_lines", s.logLines},
                     {"log", tail},
                     {"errors", s.errors},
                     {"error_count", s.errorCount},
                     {"warning_count", s.warningCount},
                     {"fatal_lines", s.fatalLines},
                     {"crashed", s.crashed},
                     {"exit_code", s.exitCode ? Json(*s.exitCode) : Json(nullptr)}};
        if (!deployed.url.empty()) data["device"]["url"] = deployed.url;
        if (stoppedHere) data["stopped_by_run_build"] = true;
        if (keepRunning && s.running) data["next"] = "it keeps running: read_console categories [\"device\"] for its log";
        if (shots.size() == 1 && wantShot && !args.contains("screenshots_at") && shots[0].contains("image")) data["screenshot"] = shots[0];
        else if (!shots.empty()) data["screenshots"] = shots;
        std::string text = device.name + ": " + (launchOnly ? "launched" : deployed.installed == "patched" ? "patched (" + std::to_string(deployed.pushedFiles) +
                           " files) and launched" : "installed and launched") + ", " +
                           (s.crashed ? "crashed" : stoppedHere ? "stopped after " + std::to_string(static_cast<int>(seconds)) + " s" :
                            s.running ? "still running" : "exited") + ". " + std::to_string(s.logLines) + " log line(s)";
        if (s.errorCount) text += ", " + std::to_string(s.errorCount) + " ERROR(S): " + s.errors.front();
        if (!s.fatalLines.empty()) text += ", fatal: " + s.fatalLines.front();
        text += ".";
        if (!result.images.empty()) text += " " + std::to_string(result.images.size()) + " screenshot(s).";
        result.text = text;
        result.structured = data;
        result.isError = false;   // a crash is a finding about the game, not a tool failure
        outcome->done = true;
    }).detach();
    call->waitUntil([outcome]() { return outcome->done.load(); }, static_cast<int>((seconds + 900.0) * 1000.0), [call, outcome](bool timedOut) {
        if (timedOut) {
            call->fail("run_build device did not finish: the device stopped answering (get_build_info devices:true).");
            return;
        }
        call->reply(outcome->result);
    });
}
