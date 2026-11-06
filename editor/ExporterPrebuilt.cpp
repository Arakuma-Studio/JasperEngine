// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Player builds (docs/specs/S30-shipping.md D1, R5-R7, R12-R15): the method a project needs,
// then either the prebuilt steps (the content steps every build runs, the cooked scenes,
// game.json, the player runtime staged next to them, notices, the content manifest, the
// platform's finalizer) or the Compile build of today's Desktop and Web exports. Everything is
// written to <dest>.staging-<pid> and swapped in at the end.

#include "Exporter.h"

#include "EditorHost.h"
#include "Generator.h"
#include "Out.h"
#include "cook/JPakWriter.h"
#include "io/JPak.h"
#include "render/shadow/ShadowTiers.h"
#include "io/AtomicFile.h"
#include "platform/AndroidPackager.h"
#include "platform/IosProject.h"
#include "platform/MacBundle.h"
#include "platform/WindowsResources.h"
#include "player/ContentManifest.h"
#include "player/PlayerConfig.h"
#include "player/PlayerModules.h"
#include "players/PlayerRuntimes.h"
#include "publish/Notices.h"
#include "publish/PayloadValidator.h"
#include "serialization/CookedScene.h"
#include "util/FileUtils.h"
#include "util/Sha256.h"
#include "EditorVersion.h"

#include <algorithm>
#include <fstream>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool renameRetry(const fs::path& from, const fs::path& to) {
        std::error_code error;
        return io::renameWithRetry(from, to, error);
    }

    bool writeText(const fs::path& path, const std::string& text, std::string& error) {
        const io::WriteResult written = io::writeFileAtomic(path, text);
        if (!written.ok()) {
            error = io::describeWriteError(written);
            return false;
        }
        return true;
    }

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    // a tree copied over another (files replaced, the rest kept)
    bool copyInto(const fs::path& from, const fs::path& to, std::string& error) {
        std::error_code ec;
        if (!fs::exists(from, ec)) return true;
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            error = "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    // the content-relative paths of every regular file below `root`
    std::vector<std::string> filesBelow(const fs::path& root) {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) out.push_back(fs::relative(it->path(), root, ec).generic_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

}

editor::ExportResult editor::Exporter::getResult() const {
    std::lock_guard<std::mutex> lock(progressMutex);
    return result;
}

void editor::Exporter::warn(const std::string& code, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        result.warnings.push_back({code, message});
    }
    Out::warning("%s: %s", code.c_str(), message.c_str());
}

std::string editor::Exporter::playerIdFor(const ExportConfig& cfg, bool development) {
    std::string backend = normalizeBackend(cfg.graphicBackend);
    if (backend.empty()) backend = platformBackends(cfg.platform).front();
    return PlayerRuntimes::runtimeId(buildPlatformName(cfg.platform), platformArch(cfg.platform), backend, development);
}

namespace {

    // a source as a reason names it: project-relative inside the project, else as it is
    std::string projectRelativeName(const Project* project, const fs::path& path) {
        const fs::path root = project->getProjectPath();
        if (!root.empty()) {
            const fs::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
            if (!relative.empty() && *relative.begin() != "..") return relative.generic_string();
        }
        return path.lexically_normal().generic_string();
    }

}

editor::BuildMethodChoice editor::Exporter::chooseBuildMethod(Project* proj, const ExportConfig& cfg, const PlayerInfo* playerInfo) {
    BuildMethodChoice choice;
    // (a) C++ scripts: what a script directory compiles (Generator::writeSourceFiles)
    std::vector<std::string> sources;
    for (const fs::path& scriptDir : proj->getScriptDirs()) {
        const fs::path root = scriptDir.is_absolute() ? scriptDir : proj->getProjectPath() / scriptDir;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec) && (name.empty() || name[0] == '.' || name == "build")) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && isCppSourceFile(it->path()) && !isCppHeaderFile(it->path()))
                sources.push_back(projectRelativeName(proj, it->path()));
        }
    }
    // and the C++ scripts the scenes and the prefabs scripts instantiate use (what the Compile
    // path compiles: Exporter::copyCppScripts), wherever they are
    for (const SceneScriptSource& script : proj->getBuildCppScripts()) {
        if (script.path.empty()) continue;
        const fs::path source = script.path.is_absolute() ? script.path : proj->getProjectPath() / script.path;
        const std::string name = projectRelativeName(proj, source);
        if (std::find(sources.begin(), sources.end(), name) == sources.end()) sources.push_back(name);
    }
    std::sort(sources.begin(), sources.end());
    if (!sources.empty()) {
        std::string list;
        for (size_t i = 0; i < sources.size() && i < 3; i++) list += (i ? ", " : "") + sources[i];
        if (sources.size() > 3) list += ", +" + std::to_string(sources.size() - 3) + " more";
        choice.reasons.push_back("C++ scripts (" + list + ")");
    }
    // (b) the user's build file
    std::error_code ec;
    if (fs::exists(proj->getProjectPath() / "ProjectBuild.cmake", ec)) choice.reasons.push_back("ProjectBuild.cmake");
    // (c) capacities above the player's (the engine defaults when no player is known)
    for (const auto& [macro, value] : sceneMaxValuesOf(proj)) {
        unsigned int capacity = 0;
        if (playerInfo) {
            auto it = playerInfo->capacities.find(macro);
            if (it != playerInfo->capacities.end()) capacity = it->second;
        }
        if (capacity == 0) {
            static const std::map<std::string, unsigned int> defaults = {
                {"MAX_SUBMESHES", MAX_SUBMESHES}, {"MAX_TILEMAP_TILESRECT", MAX_TILEMAP_TILESRECT}, {"MAX_TILEMAP_TILES", MAX_TILEMAP_TILES},
                {"MAX_SPRITE_FRAMES", MAX_SPRITE_FRAMES}, {"MAX_EXTERNAL_BUFFERS", MAX_EXTERNAL_BUFFERS}, {"MAX_BONES", MAX_BONES}};
            // (no collider capacities since upstream's unbounded collider lists)
            auto it = defaults.find(macro);
            capacity = it != defaults.end() ? it->second : value;
        }
        if (value > capacity)
            choice.reasons.push_back(macro + " " + std::to_string(value) + " (the player holds " + std::to_string(capacity) + ")");
    }
    // (d) a compile-time feature the player lacks: Google Mobile Ads
    const bool ads = (cfg.platform == BuildPlatform::Android && proj->getAndroidProjectSettings().admobEnabled) ||
                     (cfg.platform == BuildPlatform::IOS && proj->getIOSProjectSettings().admobEnabled);
    if (ads && !(playerInfo && std::find(playerInfo->features.begin(), playerInfo->features.end(), "ads") != playerInfo->features.end()))
        choice.reasons.push_back("Google Mobile Ads (AdMob)");
    // (e) forced
    if (cfg.method == BuildMethod::Compile) choice.reasons.push_back("Compile chosen in Build Settings");
    choice.method = choice.reasons.empty() ? BuildMethod::Prebuilt : BuildMethod::Compile;
    return choice;
}

bool editor::Exporter::resolvePlayer() {
    // the full player, else any installed variant of it (the method only needs one: every variant
    // has the same capacities); selectPlayerVariant picks the build's once its modules are known
    const std::string id = playerIdFor(config, config.developmentBuild);
    std::string error;
    std::optional<PlayerInfo> found = PlayerRuntimes::find(id, config.playersDir, config.allowPlayerMismatch, error);
    if (!found) {
        std::vector<PlayerInfo> variants = PlayerRuntimes::findVariants(id, config.playersDir, config.allowPlayerMismatch);
        if (!variants.empty()) found = variants.front();
    }
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        result.playerId = id;
    }
    if (!found) {
        player.reset();
        return false;
    }
    player = std::make_shared<PlayerInfo>(*found);
    if (config.allowPlayerMismatch && player->engineVersion != PlayerRuntimes::editorVersion())
        warn("W-PLAYER-MISMATCH", "player " + id + " is for editor " + player->engineVersion + ", this is " + PlayerRuntimes::editorVersion() +
                                  " (--allow-player-mismatch; publish verify refuses the build)");
    return true;
}

void editor::Exporter::runPlayerBuild() {
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        result = ExportResult();
        result.outputDir = config.destinationDir;
    }
    if (project) config.developmentBuild = config.developmentBuild || project->isDevelopmentBuild();
    const std::string backend = normalizeBackend(config.graphicBackend);
    const std::vector<std::string> backends = platformBackends(config.platform);
    if (!config.graphicBackend.empty() && (backend.empty() || std::find(backends.begin(), backends.end(), backend) == backends.end())) {
        std::string list;
        for (const std::string& b : backends) list += (list.empty() ? "" : ", ") + b;
        setError("graphics API " + config.graphicBackend + " has no " + buildPlatformName(config.platform) + " player (use " + list + ")");
        return;
    }
    config.graphicBackend = backend.empty() ? backends.front() : backend;

    resolvePlayer();
    BuildMethodChoice choice = chooseBuildMethod(project, config, player.get());
    // Auto without the player runtime: a build this computer can compile is made from source
    // (an engine checkout, or a release editor before its runtimes are downloaded), else the
    // missing runtime is the error below
    if (choice.method == BuildMethod::Prebuilt && !player && config.method == BuildMethod::Auto) {
        const bool canCompile = config.platform == BuildPlatform::Web ? detectEmsdk(config.emsdkPath).found
                                : config.platform == hostBuildPlatform() && Generator::checkBuildTools().empty();
        if (canCompile) {
            choice.method = BuildMethod::Compile;
            choice.reasons.push_back("no " + playerIdFor(config, config.developmentBuild) + " player runtime installed");
        }
    }
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        result.compileReasons = choice.reasons;
        result.methodUsed = choice.method;
    }
    Out::info("Build method: %s%s", choice.method == BuildMethod::Prebuilt ? "prebuilt player" : "compile from source",
              choice.reasons.empty() ? "" : (": " + choice.reasons.front()).c_str());

    if (config.method == BuildMethod::Prebuilt && !choice.reasons.empty()) {
        std::string reasons;
        for (const std::string& reason : choice.reasons) reasons += "\n  - " + reason;
        setError("this project cannot use a prebuilt player:" + reasons);
        return;
    }

    if (choice.method == BuildMethod::Compile) {
        const bool hostDesktop = config.platform == hostBuildPlatform();
        if (!hostDesktop && config.platform != BuildPlatform::Web) {
            setError(std::string("needs a C++ toolchain (") + choice.reasons.front() + "): a " + buildPlatformName(config.platform) +
                     " build of native code is made from the source project (File > Build Settings > Export source, then " +
                     (config.platform == BuildPlatform::Android ? "Android Studio" : config.platform == BuildPlatform::IOS ? "Xcode" : "CMake") + ")");
            return;
        }
        if (config.platform != BuildPlatform::Web) {
            const std::string toolchain = Generator::checkBuildTools();
            if (!toolchain.empty()) {
                setError("needs a C++ toolchain (" + choice.reasons.front() + "): " + toolchain);
                return;
            }
        } else if (!detectEmsdk(config.emsdkPath).found) {
            setError("needs a C++ toolchain (" + choice.reasons.front() + "): the web build of native code needs emsdk (EMSDK or emcc on PATH)");
            return;
        }
        config.mode = config.platform == BuildPlatform::Web ? ExportMode::Web : ExportMode::Desktop;
        config.selectedBackends = {shaderBackendOf(config.platform, config.graphicBackend)};
        config.webMultithreaded = config.webMultithreaded || project->getThreadingSettings().webMultithreaded;
        holdFinish = true;
        runExport();
        holdFinish = false;
        if (getProgress().failed) return;
        // a Compile build ships the same notices and manifest, so publish treats both alike
        std::string error;
        std::vector<std::string> staged;
        const std::vector<std::string> compiled = compiledModules();
        reportModules(compiled);
        if (!publish::stageCompileNotices(config.destinationDir, compiled, staged, error)) {
            setError("Notices: " + error);
            return;
        }
        for (const std::string& name : staged) {
            const size_t suffix = name.rfind("-LICENSE");
            if (suffix != std::string::npos && suffix + 8 == name.size()) compileNotices.push_back(name.substr(0, suffix));
        }
        contentDir = config.destinationDir;
        stagingDir = config.destinationDir;
        // the executable shipped stripped; its symbols (the build tree's symbols/) kept per build
        const fs::path executable = config.destinationDir / (getAppName() + (config.platform == BuildPlatform::Windows ? ".exe" : ""));
        if (config.mode == ExportMode::Desktop && !keepBuildSymbols(executable)) return;
        if (!writeContentManifest(false)) return;
        std::lock_guard<std::mutex> lock(progressMutex);
        result.methodUsed = BuildMethod::Compile;
        result.contentManifest = config.destinationDir / "content-manifest.json";
        std::error_code exists;
        if (config.mode == ExportMode::Desktop && fs::is_regular_file(executable, exists)) result.executable = executable;
        // only now: a caller reading the result once the build is finished finds all of it
        progress.finished = true;
        return;
    }

    if (!player) {
        std::string error;
        PlayerRuntimes::find(playerIdFor(config, config.developmentBuild), config.playersDir, config.allowPlayerMismatch, error);
        // what would build it without the runtime
        if (config.platform == BuildPlatform::Web && config.method == BuildMethod::Auto)
            error += "; or install the Emscripten SDK (EMSDK, or Preferences > External Tools) to compile it";
        else if (config.platform == hostBuildPlatform() && config.method == BuildMethod::Auto)
            error += "; or install a C++ toolchain to compile it (" + Generator::checkBuildTools() + ")";
        setError(error);
        return;
    }
    if (!runPrebuilt()) {
        discardStaging();
        return;
    }
    measureBuildEngine(config.destinationDir);
    if (!writeBuildReport(config.destinationDir, "prebuilt")) return;
    setProgressRaw("Build complete", 1.0f);
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        progress.finished = true;
    }
    Out::info("Build completed: %s", config.destinationDir.string().c_str());
}

fs::path editor::Exporter::shaderlibSourceDir() const {
    std::error_code ec;
    if (fs::is_directory(config.targetDir / "shaderlib", ec)) return config.targetDir / "shaderlib";
    return FileUtils::getEngineDir() / "shaderlib";
}

fs::path editor::Exporter::engineSourceDir() const {
    // a Compile export copied the engine next to it; a prebuilt build has none: the editor's own
    std::error_code ec;
    if (fs::is_directory(config.targetDir / "core", ec)) return config.targetDir;
    return FileUtils::getEngineDir();
}

void editor::Exporter::discardStaging() {
    if (stagingDir.empty() || stagingDir == config.destinationDir) return;
    std::error_code ec;
    fs::remove_all(stagingDir, ec);
}

bool editor::Exporter::selectPlayerVariant() {
    if (!moduleUsageKnown) {
        setError("the modules the project needs are not known (its scenes did not load)");
        return false;
    }
    const std::vector<std::string> needed = moduleUsage.list();
    const std::string baseId = playerIdFor(config, config.developmentBuild);
    const std::vector<PlayerInfo> installed = PlayerRuntimes::findVariants(baseId, config.playersDir, config.allowPlayerMismatch);
    const PlayerInfo* best = PlayerRuntimes::smallestWith(installed, needed);
    // (a development build is always the full player: its variants are release ones)
    const PlayerVariant& fitting = config.developmentBuild ? PlayerRuntimes::variants().front() : PlayerRuntimes::variantFor(needed);
    const std::string fittingId = fitting.suffix.empty() ? baseId : baseId + "-" + fitting.suffix;
    if (!best) {
        std::string names;
        for (const std::string& module : needed) names += (names.empty() ? "" : " ") + module;
        setError("no installed " + baseId + " player runtime has the modules this project needs (" + names + "): install " + fittingId +
                 (PlayerRuntimes::isReleaseEditor() ? " (Build Settings > Download)" : " (tools/build.sh players)"));
        return false;
    }
    // only when a variant of fewer modules than the chosen one holds the project and is missing
    // (of two that hold it with as many modules, the installed one is as good)
    const size_t bestModules = std::count_if(player::allModules().begin(), player::allModules().end(), [&](const std::string& module) {
        return std::find(best->features.begin(), best->features.end(), module) != best->features.end();
    });
    const bool fittingInstalled = std::any_of(installed.begin(), installed.end(), [&](const PlayerInfo& info) { return info.id == fittingId; });
    if (!fittingInstalled && fitting.modules.size() < bestModules && best->shippedBytes > 0) {
        warn("W-PLAYER-VARIANT", "the smaller player " + fittingId + " fits this project (modules: " + [&]() {
                 std::string list;
                 for (const std::string& module : fitting.modules) list += (list.empty() ? "" : " ") + module;
                 return list;
             }() + ") but is not installed: the build uses " + best->id + ". Install it (Build Settings > Download, `jasperengine-editor players download " +
             fittingId + "`) for a smaller game.");
    }
    player = std::make_shared<PlayerInfo>(*best);
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        result.playerId = player->id;
    }
    std::vector<std::string> shipped;
    for (const std::string& module : player::allModules())
        if (std::find(player->features.begin(), player->features.end(), module) != player->features.end()) shipped.push_back(module);
    reportModules(shipped);
    return true;
}

void editor::Exporter::reportModules(const std::vector<std::string>& shipped) {
    std::vector<std::string> leftOut;
    for (const std::string& module : player::allModules())
        if (std::find(shipped.begin(), shipped.end(), module) == shipped.end()) leftOut.push_back(module);
    std::string kept, out;
    for (const std::string& module : shipped) {
        const std::string why = moduleUsage.firstEvidence(module);
        kept += (kept.empty() ? "" : ", ") + module + (why.empty() ? std::string() : " (" + why + ")");
    }
    for (const std::string& module : leftOut) out += (out.empty() ? "" : ", ") + module;
    Out::info("Engine modules: %s; left out: %s", kept.empty() ? "none" : kept.c_str(), out.empty() ? "none" : out.c_str());
    std::lock_guard<std::mutex> lock(progressMutex);
    result.modules = shipped;
    result.modulesLeftOut = leftOut;
    result.moduleEvidence = moduleUsage.evidence;
}

bool editor::Exporter::runPrebuilt() {
    progressScale = 1.0f;
    if (config.destinationDir.empty()) {
        setError("Destination directory not specified");
        return false;
    }
    config.destinationDir = fs::absolute(config.destinationDir).lexically_normal();
    // the content steps write into the build cache (its project/ is rewritten every build)
    config.targetDir = project->getProjectInternalPath() / "export" / ("player-" + std::string(buildPlatformName(config.platform)));
    std::error_code ec;
    fs::create_directories(config.targetDir, ec);
    fs::remove_all(getExportProjectRoot(), ec);
    fs::remove_all(config.targetDir / "shaders", ec);
    fs::remove_all(config.targetDir / "licenses", ec);
    if (ec) {
        setError("Failed to clear the build cache: " + ec.message());
        return false;
    }
    config.shaderOutputFormat = ShaderOutputFormat::Binary;
    config.selectedBackends = {shaderBackendOf(config.platform, config.graphicBackend)};

    // R7: the old build stays as it is until the new one is complete
    stagingDir = config.destinationDir.parent_path() / (config.destinationDir.filename().string() + ".staging-" + std::to_string(io::currentProcessId()));
    fs::remove_all(stagingDir, ec);
    fs::create_directories(stagingDir, ec);
    if (ec) {
        setError("Failed to create " + stagingDir.string() + ": " + ec.message());
        return false;
    }
    // where the content lives inside the staged build
    const std::string app = getAppName();
    switch (config.platform) {
        case BuildPlatform::MacOS: contentDir = stagingDir / (app + ".app") / "Contents" / "Resources"; break;
        case BuildPlatform::IOS: contentDir = stagingDir / app / "Content"; break;
        case BuildPlatform::Android:
        case BuildPlatform::Web: contentDir = config.targetDir / "content"; break;
        default: contentDir = stagingDir; break;
    }
    fs::remove_all(config.targetDir / "content", ec);
    fs::create_directories(contentDir, ec);

    auto cancelled = [this]() {
        if (!isCancelled()) return false;
        setError("Build cancelled");
        return true;
    };

    if (!loadAndSaveAllScenes()) return false;
    if (!checkMissingAssets()) return false;
    if (!selectPlayerVariant()) return false;
    resolveShaderKeys();
    if (cancelled()) return false;
    if (!copyLua()) return false;
    if (!checkHtmlUIReferences()) return false;
    checkSoundEventClips();
    if (cancelled()) return false;
    if (!cookTextures()) return false;
    if (cancelled()) return false;
    if (!cookModels()) return false;
    if (cancelled()) return false;
    if (!copyAssets()) return false;
    if (!writeAudioImports()) return false;
    if (!writeAssetMap()) return false;
    if (!copyCustomShaderSources()) return false;
    if (cancelled()) return false;
    if (!buildAndSaveShaders()) return false;
    if (!writeEffectLicense()) return false;
    if (cancelled()) return false;

    // R6 steps 2-8
    if (!cookScenes()) return false;
    if (cancelled()) return false;
    if (!writeGameConfig()) return false;
    if (!stagePlayer()) return false;
    if (cancelled()) return false;
    if (!stageNotices()) return false;
    // what ships, measured before the pack hides it (S53 R21)
    buildReport = BuildReport();
    measureBuildContent(contentDir);
    if (!packContent()) return false;
    if (cancelled()) return false;
    if (!writeContentManifest(true)) return false;
    if (cancelled()) return false;

    bool finalized = false;
    switch (config.platform) {
        case BuildPlatform::Linux: finalized = finalizeLinux(); break;
        case BuildPlatform::Windows: finalized = finalizeWindows(); break;
        case BuildPlatform::MacOS: finalized = finalizeMacOS(); break;
        case BuildPlatform::Web: finalized = finalizeWeb(); break;
        case BuildPlatform::Android: finalized = finalizeAndroid(); break;
        case BuildPlatform::IOS: finalized = finalizeIOS(); break;
    }
    if (!finalized) return false;
    if (cancelled()) return false;

    // R30: what the build made is what a release may hold
    setProgressRaw("Checking the build...", 0.97f);
    publish::ValidationTarget target;
    target.platform = config.platform;
    target.arch = platformArch(config.platform);
    target.backend = config.graphicBackend;
    target.executable = fs::relative(getResult().executable, stagingDir, ec).generic_string();
    std::vector<std::string> findings;
    publish::validatePayload(stagingDir, target, findings);
    for (const std::string& finding : findings) {
        // an unsigned macOS build is the W-MAC-UNSIGNED warning here; publish refuses it
        if (finding.find("unsigned") != std::string::npos && config.platform == BuildPlatform::MacOS) continue;
        setError("The built player does not pass the payload check: " + finding);
        return false;
    }
    return swapStagingIntoPlace();
}

bool editor::Exporter::cookScenes() {
    setProgressRaw("Cooking scenes...", 0.75f);
    for (const cook::CookedFileBytes& file : cookedFiles) {
        const fs::path target = contentDir / editor::FileUtils::pathFromUtf8(file.relativePath);
        const io::WriteResult written = io::writeFileAtomic(target, std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()));
        if (!written.ok()) {
            setError("Failed to write " + target.string() + ": " + io::describeWriteError(written));
            return false;
        }
    }
    Out::info("Cooked %zu scenes, prefabs and default-sky faces", cookedFiles.size());
    // the content trees the steps before made (assets, lua, the shaders under assets/shaders)
    std::string error;
    if (!copyInto(getExportProjectRoot() / "assets", contentDir / "assets", error) ||
        !copyInto(getExportProjectRoot() / "lua", contentDir / "lua", error) ||
        !copyInto(config.targetDir / "shaders", contentDir / "assets" / "shaders", error) ||
        !copyInto(config.targetDir / "licenses", contentDir / "licenses", error)) {
        setError(error);
        return false;
    }
    // a build of one platform ships the texture families it loads only (S14 R9)
    for (const std::string family : {"desktop", "mobile", "android_etc2", "web"}) {
        const bool keep = (family == std::string("desktop") && isDesktopPlatform(config.platform)) ||
                          (family == std::string("web") && config.platform == BuildPlatform::Web) ||
                          ((family == std::string("mobile") || family == std::string("android_etc2")) &&
                           (config.platform == BuildPlatform::Android || config.platform == BuildPlatform::IOS)) ;
        if (!keep) {
            std::error_code ec;
            fs::remove_all(contentDir / "assets" / "je-cooked" / family, ec);
        }
    }
    return true;
}

bool editor::Exporter::writeGameConfig() {
    setProgressRaw("Writing game.json...", 0.78f);
    player::PlayerConfig game;
    const WindowSettings window = project->getWindowSettings();
    game.name = project->getName().empty() ? getAppName() : project->getName();
    game.appId = config.platform == BuildPlatform::Android ? project->getAndroidProjectSettings().packageName
               : config.platform == BuildPlatform::MacOS ? project->getMacOSProjectSettings().bundleIdentifier
               : config.platform == BuildPlatform::IOS ? project->getIOSProjectSettings().bundleIdentifier
               : getAppName();
    if (game.appId.empty()) game.appId = getAppName();
    game.version = project->getAndroidProjectSettings().versionName;
    // the selected scene of the editor (in Prefab Mode, the scene behind it), else the project's
    game.startScene = config.startSceneId != 0 ? config.startSceneId : project->getStartSceneId();
    for (const SceneProject& scene : project->getScenes()) {
        player::GameScene item;
        item.id = scene.id;
        item.name = scene.name;
        item.file = "scenes/" + std::to_string(scene.id) + ".jscn";
        project->collectInvolvedScenesForExport(scene.id, item.involved);
        project->collectStartActiveScenesForExport(scene.id, item.active);
        game.scenes.push_back(std::move(item));
    }
    if (!game.findScene(game.startScene) && !game.scenes.empty()) game.startScene = game.scenes.front().id;
    for (const cook::CookedPrefabInfo& prefab : cookedPrefabs) game.prefabs.push_back({prefab.id, prefab.name, prefab.guid, prefab.file});
    game.windowMode = static_cast<int>(window.mode);
    game.windowWidth = static_cast<int>(window.width);
    game.windowHeight = static_cast<int>(window.height);
    game.resizable = window.resizable;
    game.title = window.title.empty() ? game.name : window.title;
    game.vsync = project->isVSyncEnabled();
    game.storageId = Factory::toIdentifier(game.name);
    game.canvasWidth = project->getCanvasWidth();
    game.canvasHeight = project->getCanvasHeight();
    game.scaling = project->getScalingMode();
    game.textureStrategy = project->getTextureStrategy();
    const LoadingSettings& loading = project->getLoadingSettings();
    game.loadingScene = loading.sceneId;
    game.loadingDelay = loading.delay;
    game.loadingTimeout = loading.timeout;
    game.asyncLoading = loading.asyncLoading;
    const ThreadingProjectSettings& threading = project->getThreadingSettings();
    game.threading.workers = threading.workers;
    game.threading.reservedFrameWorkers = threading.reservedFrameWorkers;
    game.threading.ioThreads = threading.ioThreads;
    game.threading.webPumpBudgetMs = threading.webPumpBudgetMs;
    game.atmosphereQuality = project->getAtmosphereQuality();
    game.inputDeadzone = project->getInputDeadzone();
    game.inputActions = project->getInputActionsEnginePath();
    game.navigation = project->getNavigationSettings().settings;
    const TextureProjectSettings& textures = project->getTextureSettings();
    for (size_t g = 0; g < 3; g++)
        game.textureDefaults[g] = {textures.textureQuality[g], textures.memoryBudgetMB[g], textures.lowMemoryFallback[g]};
    game.htmlUiMsaa = project->getHtmlUiMsaaSamples();
    game.glyphAtlasMiB = project->getHtmlUiRenderSettings().glyphAtlasMiB;
    game.sdfPrimitives = project->getHtmlUiRenderSettings().sdfPrimitives;
    game.transformAwareText = project->getHtmlUiRenderSettings().transformAwareText;
    game.mobilePbrTier = project->getMobilePbrTier();
    game.dynamicBatching = project->isDynamicBatching();
    game.lightMode = project->getLightMode();
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) game.shadowTiers[platform] = ShadowTiers::getPlatformTier(platform);
    game.device = project->getDeviceSettings();
    game.tags = project->getTags();
    game.layerNames = project->getLayerNames();
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        game.collision3D[layer] = project->getLayerCollisionMatrix3D()[layer];
        game.collision2D[layer] = project->getLayerCollisionMatrix2D()[layer];
    }
    const PlayerLoggingSettings& logging = project->getPlayerLogging();
    game.logLevel = logging.level;
    game.logFile = logging.fileLog;
    game.logBacktrace = logging.backtrace;
    const PlayerCrashSettings& crash = project->getPlayerCrash();
    game.crashHandler = crash.enabled;
    game.crashLogTail = crash.includeLogTail;
    game.crashDumpWithHeap = crash.dumpWithHeap;
    game.hangWatchdog = crash.hangWatchdog;
    game.hangMs = crash.hangSeconds * 1000;
    game.crashWebOverlay = crash.webCrashOverlay;
    // Project Settings > Audio, as Play applies them (Project::setAudioSettings) and a Compile
    // build's init (Generator::audioInitSource): the mixer by its asset-relative path, which ships
    const AudioSettings& audio = project->getAudioSettings();
    game.audioMixer = audio.mixer;
    game.audioMaxRealVoices = audio.maxRealVoices;
    game.audioMaxVoices = audio.maxVoices;
    game.audioSeed = audio.seed;
    game.audioMusicGroup = audio.musicGroup;
    game.audioSampleRate = audio.sampleRate;
    game.audioOutputStage = audio.outputStage;
    game.audioIosSession = audio.iosSessionCategory;

    // the project icon, at the content root: the window and taskbar icon of every platform
    fs::path icon = project->getWindowIcon();
    if (!icon.empty() && icon.is_relative()) icon = project->getProjectPath() / icon;
    std::error_code ec;
    if (!icon.empty() && fs::is_regular_file(icon, ec) && icon.extension() == ".png") {
        fs::copy_file(icon, contentDir / "icon.png", fs::copy_options::overwrite_existing, ec);
        if (!ec) game.icon = "icon.png";
    }
    std::string error;
    if (!writeText(contentDir / "game.json", game.toJson().dump(2) + "\n", error)) {
        setError("game.json: " + error);
        return false;
    }
    return true;
}

bool editor::Exporter::stagePlayer() {
    setProgressRaw("Staging the player...", 0.8f);
    std::string error;
    // what is staged as it is (a desktop executable, the web runtime, a macOS bundle template)
    if (!copyInto(player->dir / "files", stagingDir, error)) {
        setError("Player " + player->id + ": " + error);
        return false;
    }
    if (config.platform == BuildPlatform::MacOS) {
        // the template bundle takes the game's name; the content goes into its Resources
        std::error_code ec;
        const fs::path templateApp = stagingDir / "JasperEnginePlayer.app";
        const fs::path app = stagingDir / (getAppName() + ".app");
        if (fs::exists(templateApp, ec)) {
            if (fs::exists(app, ec)) {
                if (!copyInto(templateApp, app, error)) {
                    setError(error);
                    return false;
                }
                fs::remove_all(templateApp, ec);
            } else {
                fs::rename(templateApp, app, ec);
            }
        }
        if (ec) {
            setError("Failed to stage the macOS bundle: " + ec.message());
            return false;
        }
    }
    return true;
}

bool editor::Exporter::stageNotices() {
    setProgressRaw("Staging notices...", 0.82f);
    // the player runtime's own (CI wrote them for its libraries), plus the shaders' notices the
    // content steps wrote (writeEffectLicense)
    std::string error;
    if (!copyInto(player->dir / "licenses", contentDir / "licenses", error)) {
        setError("Notices: " + error);
        return false;
    }
    std::error_code ec;
    if (fs::is_regular_file(player->dir / "THIRD-PARTY-NOTICES.txt", ec))
        fs::copy_file(player->dir / "THIRD-PARTY-NOTICES.txt", contentDir / "THIRD-PARTY-NOTICES.txt", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        setError("Notices: " + ec.message());
        return false;
    }
    for (const std::string& library : player->notices) {
        if (!fs::is_regular_file(contentDir / "licenses" / (library + "-LICENSE"), ec)) {
            setError("Notices: the player lists " + library + " but its runtime has no licenses/" + library + "-LICENSE");
            return false;
        }
    }
    return true;
}

bool editor::Exporter::packContent() {
    const bool pack = config.packNativeResources || config.platform == BuildPlatform::Web;
    if (!pack) return true;
    setProgressRaw("Packing content...", 0.85f);
    // a desktop pack sits next to the executable; the notices stay loose (they are read by people)
    const fs::path packPath = config.platform == BuildPlatform::Web ? stagingDir / "content.jpak" : contentDir / "content.jpak";
    std::vector<std::string> exclude = {"content.jpak", "content-manifest.json"};
    for (const std::string& file : filesBelow(contentDir)) {
        if (file.rfind("licenses/", 0) == 0 || file == "THIRD-PARTY-NOTICES.txt") exclude.push_back(file);
        // what the platform needs loose: the executable and its libraries, the icon of the desktop file
        if (contentDir == stagingDir && fs::path(file).parent_path().empty() && file != "game.json" && file != "icon.png")
            exclude.push_back(file);
    }
    cook::JPakStats stats;
    std::string error;
    if (!cook::writeJPak(contentDir, packPath, exclude, stats, error)) {
        setError("Content pack: " + error);
        return false;
    }
    // the packed files go: the player reads them from the pack
    std::error_code ec;
    for (const std::string& file : filesBelow(contentDir)) {
        if (std::find(exclude.begin(), exclude.end(), file) != exclude.end()) continue;
        fs::remove(contentDir / editor::FileUtils::pathFromUtf8(file), ec);
    }
    for (const char* dir : {"assets", "lua", "scenes", "prefabs"}) fs::remove_all(contentDir / dir, ec);
    Out::info("Content pack: %u files, %llu bytes", stats.entries, (unsigned long long)stats.bytes);
    return true;
}

bool editor::Exporter::keepBuildSymbols(const fs::path& executable) {
    // a release build's link split them off (JASPERENGINE_SPLIT_SYMBOLS); a Debug build keeps
    // its own in the binary
    buildSymbolsDir.clear();
    const fs::path built = config.targetDir / "build" / "symbols";
    std::error_code ec;
    if (!fs::is_directory(built, ec) || !fs::is_regular_file(executable, ec)) return true;
    // under the executable's hash: a later build cannot replace the symbols a release of this one
    // needs; the five newest are kept
    std::string hex, error;
    if (!sha256File(executable, hex, &error)) {
        setError("Symbols: " + error);
        return false;
    }
    const fs::path root = project->getProjectInternalPath() / "symbols";
    const fs::path dir = root / hex.substr(0, 16);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::copy(built, dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        setError("Symbols: cannot copy " + built.string() + " to " + dir.string() + ": " + ec.message());
        return false;
    }
    std::vector<std::pair<fs::file_time_type, fs::path>> kept;
    for (const auto& entry : fs::directory_iterator(root, ec))
        if (entry.is_directory(ec)) kept.emplace_back(fs::last_write_time(entry.path(), ec), entry.path());
    std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 5; i < kept.size(); i++)
        if (kept[i].second != dir) fs::remove_all(kept[i].second, ec);
    buildSymbolsDir = fs::absolute(dir, ec);
    return true;
}

bool editor::Exporter::writeContentManifest(bool prebuilt) {
    setProgressRaw("Writing the content manifest...", 0.88f);
    player::ContentManifest manifest;
    manifest.engineVersion = JASPERENGINE_EDITOR_VERSION;
    manifest.contentFormatVersion = ::jasperengine::cook::contentFormatVersion();
    manifest.method = prebuilt ? "prebuilt" : "compile";
    manifest.backend = config.graphicBackend.empty() ? getCMakeGraphicBackend(*config.selectedBackends.begin()) : normalizeBackend(config.graphicBackend);
    manifest.allowPlayerMismatch = config.allowPlayerMismatch;
    // the start scene game.json names (writeGameConfig): the first scene when the project's start
    // scene is none of its scenes (a project.yaml without startScene)
    manifest.startScene = config.startSceneId != 0 ? config.startSceneId : project->getStartSceneId();
    {
        const std::vector<SceneProject>& scenes = project->getScenes();
        const bool known = std::any_of(scenes.begin(), scenes.end(), [&](const SceneProject& scene) { return scene.id == manifest.startScene; });
        if (!known && !scenes.empty()) manifest.startScene = scenes.front().id;
    }
    // the modules the content needs (S34 R3): a player without one of them refuses to start
    manifest.modules = prebuilt ? moduleUsage.list() : compiledModules();
    manifest.hasModules = true;
    if (prebuilt && player) {
        manifest.notices = player->notices;
        manifest.playerId = player->id;
        auto exe = player->executables.find(player->executable);
        if (exe != player->executables.end()) manifest.playerSha256 = exe->second;
    }
    if (!prebuilt) {
        manifest.notices = compileNotices;
        manifest.symbols = buildSymbolsDir.empty() ? std::string() : buildSymbolsDir.generic_string();
    }
    for (const cook::CookedFileBytes& file : cookedFiles) {
        if (file.relativePath.rfind("scenes/", 0) == 0) manifest.scenes.push_back(file.relativePath);
        else if (file.relativePath.rfind("prefabs/", 0) == 0) manifest.prefabs.push_back(file.relativePath);
    }
    // the shaders the build made for its backend
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(config.targetDir / "shaders", ec))
        if (entry.path().extension() == ".sdat") manifest.shaders.push_back(entry.path().filename().string());
    std::sort(manifest.shaders.begin(), manifest.shaders.end());

    const fs::path packFile = config.platform == BuildPlatform::Web ? stagingDir / "content.jpak" : contentDir / "content.jpak";
    if (prebuilt && fs::is_regular_file(packFile, ec)) {
        player::ManifestPack pack;
        pack.file = "content.jpak";
        std::string error;
        uint64_t size = 0;
        if (!sha256File(packFile, pack.sha256, &error, &size)) {
            setError(error);
            return false;
        }
        JPak reader;
        if (reader.openFile(packFile, &error)) pack.entries = reader.entries().size();
        manifest.packs.push_back(pack);
        // what the pack holds is checked there (an FNV per entry), not as loose files
        manifest.scenes.clear();
        manifest.prefabs.clear();
        manifest.shaders.clear();
    }
    // (a web player reads its pack only: the notices beside the page are not its to check)
    const bool looseFiles = !(prebuilt && config.platform == BuildPlatform::Web);
    for (const std::string& file : looseFiles ? filesBelow(contentDir) : std::vector<std::string>{}) {
        if (file == "content-manifest.json" || file == "content.jpak") continue;
        player::ManifestFile entry;
        std::string error;
        if (!sha256File(contentDir / editor::FileUtils::pathFromUtf8(file), entry.sha256, &error, &entry.size)) {
            setError(error);
            return false;
        }
        manifest.files[file] = entry;
    }
    const fs::path target = (config.platform == BuildPlatform::Web && prebuilt) ? stagingDir / "content-manifest.json" : contentDir / "content-manifest.json";
    std::string error;
    if (!writeText(target, manifest.toJson().dump(2) + "\n", error)) {
        setError("content-manifest.json: " + error);
        return false;
    }
    std::lock_guard<std::mutex> lock(progressMutex);
    result.contentManifest = target;
    return true;
}

bool editor::Exporter::swapStagingIntoPlace() {
    setProgressRaw("Publishing the build...", 0.99f);
    std::error_code ec;
    const fs::path previous = config.destinationDir.parent_path() / (config.destinationDir.filename().string() + ".previous");
    fs::remove_all(previous, ec);
    if (fs::exists(config.destinationDir, ec) && !renameRetry(config.destinationDir, previous)) {
        setError("Cannot move the previous build aside (" + config.destinationDir.string() + "): close what uses it");
        return false;
    }
    if (!renameRetry(stagingDir, config.destinationDir)) {
        if (fs::exists(previous, ec)) renameRetry(previous, config.destinationDir);
        setError("Cannot publish the build into " + config.destinationDir.string());
        return false;
    }
    fs::remove_all(previous, ec);
    // the paths of the report move with the build
    std::lock_guard<std::mutex> lock(progressMutex);
    auto moved = [&](const fs::path& path) {
        if (path.empty()) return path;
        const fs::path relative = path.lexically_relative(stagingDir);
        return (relative.empty() || *relative.begin() == "..") ? path : config.destinationDir / relative;
    };
    result.contentManifest = moved(result.contentManifest);
    result.executable = moved(result.executable);
    for (fs::path& artifact : result.artifacts) artifact = moved(artifact);
    stagingDir = config.destinationDir;
    return true;
}

// ---------------------------------------------------------------- desktop finalizers (R12)

bool editor::Exporter::finalizeLinux() {
    setProgressRaw("Finishing the Linux build...", 0.9f);
    const std::string app = getAppName();
    std::error_code ec;
    const fs::path staged = stagingDir / "jasperengine-player";
    if (!fs::is_regular_file(staged, ec)) {
        setError("player " + player->id + " has no files/jasperengine-player");
        return false;
    }
    fs::rename(staged, stagingDir / app, ec);
    if (ec) {
        setError("Failed to name the executable " + app + ": " + ec.message());
        return false;
    }
    fs::permissions(stagingDir / app, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                                          fs::perms::others_exec, fs::perm_options::replace, ec);
    const std::string title = project->getWindowSettings().title.empty() ? project->getName() : project->getWindowSettings().title;
    std::string desktop = "[Desktop Entry]\nType=Application\nName=" + title + "\nExec=" + app + "\nStartupWMClass=" + app + "\n";
    if (fs::is_regular_file(stagingDir / "icon.png", ec)) desktop += "Icon=icon\n";
    desktop += "Categories=Game;\nTerminal=false\n";
    std::string error;
    if (!writeText(stagingDir / (app + ".desktop"), desktop, error)) {
        setError(error);
        return false;
    }
    std::lock_guard<std::mutex> lock(progressMutex);
    result.executable = stagingDir / app;
    result.artifacts = {stagingDir / app, stagingDir / (app + ".desktop")};
    return true;
}

bool editor::Exporter::finalizeWindows() {
    setProgressRaw("Finishing the Windows build...", 0.9f);
    const std::string app = getAppName() + ".exe";
    std::error_code ec;
    const fs::path staged = stagingDir / "jasperengine-player.exe";
    if (!fs::is_regular_file(staged, ec)) {
        setError("player " + player->id + " has no files/jasperengine-player.exe");
        return false;
    }
    fs::rename(staged, stagingDir / app, ec);
    if (ec) {
        setError("Failed to name the executable " + app + ": " + ec.message());
        return false;
    }
    // the icon and version resources: patched on a Windows host; elsewhere the engine's icon
    // stays in the file (the window and taskbar take icon.png at runtime)
    std::string error;
    platform::WindowsVersionInfo info;
    const WindowsProjectSettings& windows = project->getWindowsProjectSettings();
    info.productName = windows.productName.empty() ? project->getName() : windows.productName;
    info.companyName = windows.companyName;
    info.fileVersion = windows.fileVersion;
    info.productVersion = windows.productVersion;
    fs::path icon = project->getWindowIcon();
    if (!icon.empty() && icon.is_relative()) icon = project->getProjectPath() / icon;
    if (platform::canPatchExecutableResources()) {
        if (!platform::patchExecutableResources(stagingDir / app, icon, info, error)) {
            setError("Windows resources: " + error);
            return false;
        }
    } else {
        warn("W-ICON-NONWINDOWS", app + " keeps the engine's icon and version resources (they are patched on a Windows host only); the "
                                  "window and taskbar icon come from icon.png at runtime");
    }
    std::lock_guard<std::mutex> lock(progressMutex);
    result.executable = stagingDir / app;
    result.artifacts = {stagingDir / app};
    return true;
}

bool editor::Exporter::finalizeMacOS() {
    setProgressRaw("Finishing the macOS build...", 0.9f);
    const std::string app = getAppName();
    const fs::path bundle = stagingDir / (app + ".app");
    platform::MacBundleSettings settings;
    const MacOSProjectSettings& mac = project->getMacOSProjectSettings();
    settings.bundleName = mac.applicationName.empty() ? project->getName() : mac.applicationName;
    settings.executable = app;
    settings.bundleIdentifier = mac.bundleIdentifier.empty() ? "com.jasperengine." + app : mac.bundleIdentifier;
    settings.version = mac.versionName.empty() ? "1.0" : mac.versionName;
    settings.build = mac.buildNumber.empty() ? "1" : mac.buildNumber;
    settings.highDpi = mac.highDpi;
    fs::path icon = mac.icon.empty() ? project->getWindowIcon() : mac.icon;
    if (!icon.empty() && icon.is_relative()) icon = project->getProjectPath() / icon;
    settings.iconPng = icon;
    std::string error;
    std::vector<BuildWarning> warnings;
    if (!platform::finishMacBundle(bundle, settings, warnings, error)) {
        setError("macOS bundle: " + error);
        return false;
    }
    for (const BuildWarning& warning : warnings) warn(warning.code, warning.message);
    std::lock_guard<std::mutex> lock(progressMutex);
    result.executable = bundle / "Contents" / "MacOS" / app;
    result.artifacts = {bundle};
    return true;
}

// ---------------------------------------------------------------- web (R15)

std::string editor::Exporter::composeWebPage(std::string page, const std::string& title, const std::string& headInclude, bool resize,
                                             bool hideUi, bool favicon, int canvasWidth, int canvasHeight, bool threads) {
    auto replaceAll = [&page](const std::string& marker, const std::string& value) {
        for (size_t at = page.find(marker); at != std::string::npos; at = page.find(marker, at + value.size())) page.replace(at, marker.size(), value);
    };
    // what the Compile build's apply.cmake adds to the head
    std::string extra = headInclude + "\n";
    if (favicon) extra += "<link rel=\"icon\" href=\"favicon.png\">\n";
    if (resize)
        extra += "<style>html, body { margin: 0; width: 100%; min-height: 100%; } #canvas { width: 100% !important; height: 100vh !important; "
                 "height: 100dvh !important; display: block; border: 0; padding: 0; }</style>\n";
    if (hideUi) extra += "<style>#status { display: none !important; }</style>\n";
    if (page.find("{{JASPERENGINE_HEAD_INCLUDE}}") == std::string::npos) {
        const size_t head = page.find("</head>");
        if (head != std::string::npos) page.insert(head, extra);
    } else {
        replaceAll("{{JASPERENGINE_HEAD_INCLUDE}}", extra);
    }
    replaceAll("{{JASPERENGINE_TITLE}}", escapeXml(title));
    replaceAll("{{JASPERENGINE_THREADS}}", threads ? "true" : "false");
    replaceAll("{{JASPERENGINE_CANVAS_WIDTH}}", std::to_string(canvasWidth));
    replaceAll("{{JASPERENGINE_CANVAS_HEIGHT}}", std::to_string(canvasHeight));
    std::string loader = "<script src=\"player-loader.js\" data-st=\"jasperengine-player-st.js\"";
    if (threads) loader += " data-mt=\"jasperengine-player-mt.js\" data-worker=\"coi-serviceworker.js\"";
    loader += " data-pack=\"content.jpak\" data-manifest=\"content-manifest.json\"></script>";
    replaceAll("{{JASPERENGINE_LOADER}}", loader);
    return page;
}

bool editor::Exporter::finalizeWeb() {
    setProgressRaw("Finishing the web build...", 0.9f);
    std::error_code ec;
    const bool threads = project->getThreadingSettings().webMultithreaded || config.webMultithreaded;
    // the pthreads player and its isolation worker ship only when the project asks for them
    if (!threads) {
        for (const char* file : {"jasperengine-player-mt.js", "jasperengine-player-mt.wasm", "jasperengine-player-mt.worker.js", "coi-serviceworker.js"})
            fs::remove(stagingDir / file, ec);
    } else if (!fs::is_regular_file(stagingDir / "jasperengine-player-mt.js", ec)) {
        setError("player " + player->id + " has no multithreaded build (jasperengine-player-mt.js): Project Settings > Player > "
                 "Threading > Web: Multithreaded needs it");
        return false;
    }
    for (const char* file : {"jasperengine-player-st.js", "jasperengine-player-st.wasm", "player-loader.js", "index.html"}) {
        if (!fs::is_regular_file(stagingDir / file, ec)) {
            setError("player " + player->id + " has no files/" + std::string(file));
            return false;
        }
    }
    // the page: the runtime's player.html (staged as index.html), wrapped in the project's own
    // shell as the Compile build wraps Emscripten's ({{JASPERENGINE_DEFAULT_HTML}})
    std::string page;
    if (!readText(stagingDir / "index.html", page)) {
        setError("Cannot read the player's index.html");
        return false;
    }
    const WebProjectSettings& web = project->getWebProjectSettings();
    if (!web.customHtmlShell.empty()) {
        const fs::path shell = web.customHtmlShell.is_absolute() ? web.customHtmlShell : project->getProjectPath() / web.customHtmlShell;
        std::string text;
        if (!readText(shell, text)) {
            setError("Cannot read the custom web HTML shell " + shell.string());
            return false;
        }
        const size_t marker = text.find("{{JASPERENGINE_DEFAULT_HTML}}");
        if (marker == std::string::npos) {
            setError("Custom web HTML shell must contain {{JASPERENGINE_DEFAULT_HTML}}");
            return false;
        }
        page = text.replace(marker, std::string("{{JASPERENGINE_DEFAULT_HTML}}").size(), page);
    }
    page = composeWebPage(page, project->getApplicationName(web.applicationName), web.headInclude, web.resizeCanvasToWindow,
                          web.hideEmscriptenUI, fs::is_regular_file(contentDir / "icon.png", ec) || !web.favicon.empty(),
                          project->getCanvasWidth(), project->getCanvasHeight(), threads);
    if (page.find("player-loader.js") == std::string::npos) {
        setError("The web page does not load player-loader.js (a custom shell keeps {{JASPERENGINE_DEFAULT_HTML}} or {{JASPERENGINE_LOADER}})");
        return false;
    }
    std::string error;
    if (!writeText(stagingDir / "index.html", page, error)) {
        setError(error);
        return false;
    }
    if (threads) {
        // hosts that read a _headers file (Netlify, Cloudflare Pages) send the isolation headers
        writeText(stagingDir / "_headers", "/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n", error);
    }
    // the notices next to the page
    if (!copyInto(contentDir / "licenses", stagingDir / "licenses", error)) {
        setError(error);
        return false;
    }
    if (fs::is_regular_file(contentDir / "THIRD-PARTY-NOTICES.txt", ec))
        fs::copy_file(contentDir / "THIRD-PARTY-NOTICES.txt", stagingDir / "THIRD-PARTY-NOTICES.txt", fs::copy_options::overwrite_existing, ec);
    if (!web.favicon.empty()) {
        const fs::path favicon = web.favicon.is_absolute() ? web.favicon : project->getProjectPath() / web.favicon;
        fs::copy_file(favicon, stagingDir / "favicon.png", fs::copy_options::overwrite_existing, ec);
        if (ec) {
            setError("Cannot copy the web favicon " + favicon.string() + ": " + ec.message());
            return false;
        }
    } else if (fs::is_regular_file(contentDir / "icon.png", ec)) {
        fs::copy_file(contentDir / "icon.png", stagingDir / "favicon.png", fs::copy_options::overwrite_existing, ec);
    }
    std::lock_guard<std::mutex> lock(progressMutex);
    result.executable = stagingDir / "index.html";
    result.artifacts = {stagingDir / "index.html", stagingDir / "content.jpak"};
    return true;
}

// ---------------------------------------------------------------- Android (R13), iOS (R14)

bool editor::Exporter::finalizeAndroid() {
    setProgressRaw("Packaging for Android...", 0.9f);
    platform::AndroidPackage package;
    const AndroidProjectSettings& android = project->getAndroidProjectSettings();
    package.settings = android;
    package.presentation = project->getPresentationSettings();
    package.label = project->getApplicationName(android.applicationName);
    if (package.label.empty()) package.label = project->getName();
    package.applicationId = project->getApplicationIdentifier(android.packageName);
    if (package.applicationId.empty()) package.applicationId = "com.jasperengine." + getAppName();
    package.versionCode = std::max(1u, project->getApplicationVersionCode(android.versionCode));
    package.versionName = project->getApplicationVersion(android.versionName);
    if (package.versionName.empty()) package.versionName = "1.0";
    package.astc = !project->getTextureSettings().androidEtc2Fallback;
    package.runtimeDir = player->dir / "android";
    package.contentDir = contentDir;
    package.projectPath = project->getProjectPath();
    package.format = config.androidFormat;
    package.debug = config.buildType == "Debug" || config.developmentBuild;
    package.keystore = android.keystorePath.empty() || android.keystorePath.is_absolute() ? android.keystorePath
                                                                                         : project->getProjectPath() / android.keystorePath;
    package.keystoreAlias = android.keystoreAlias;
    package.keystorePassword = config.keystorePassword;
    package.keyPassword = config.keyPassword;
    // an App Bundle (Google Play serves each device its ABI), one APK per selected ABI, or the
    // universal APK of every selected ABI (Project Settings > Android > Package Format)
    std::vector<std::vector<std::string>> packages;
    if (config.androidFormat == AndroidFormat::ApkPerAbi) {
        for (const std::string& abi : platform::selectedAndroidAbis(android)) packages.push_back({abi});
        if (packages.empty()) {
            setError("no Android architecture is selected (Project Settings > Android > Architectures)");
            return false;
        }
    } else {
        packages.push_back({});
    }
    std::vector<fs::path> outputs;
    std::string error;
    std::vector<BuildWarning> warnings;
    for (const std::vector<std::string>& abis : packages) {
        package.abis = abis;
        const std::string suffix = abis.empty() ? "" : "-" + abis.front();
        package.workDir = config.targetDir / ("android" + suffix);
        package.output = stagingDir / (getAppName() + suffix + (config.androidFormat == AndroidFormat::Aab ? ".aab" : ".apk"));
        if (!platform::buildAndroidPackage(package, commandRunner, [this]() { return isCancelled(); }, warnings, error)) {
            setError(error);
            return false;
        }
        outputs.push_back(package.output);
    }
    // one warning per kind, though each package of a per-ABI build gives its own
    std::set<std::string> warned;
    for (const BuildWarning& warning : warnings)
        if (warned.insert(warning.code + "\n" + warning.message).second) warn(warning.code, warning.message);
    std::lock_guard<std::mutex> lock(progressMutex);
    result.executable = outputs.front();
    result.artifacts = outputs;
    return true;
}

bool editor::Exporter::finalizeIOS() {
    setProgressRaw("Writing the Xcode project...", 0.9f);
    platform::IosProjectSettings settings;
    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    settings.appName = getAppName();
    settings.displayName = ios.applicationName.empty() ? project->getName() : ios.applicationName;
    settings.bundleIdentifier = ios.bundleIdentifier.empty() ? "com.jasperengine." + getAppName() : ios.bundleIdentifier;
    settings.version = ios.versionName.empty() ? "1.0" : ios.versionName;
    settings.build = ios.buildNumber.empty() ? "1" : ios.buildNumber;
    settings.teamId = ios.teamId;
    settings.exportMethod = ios.exportMethod;
    settings.hideStatusBar = ios.hideStatusBar;
    fs::path icon = ios.icon.empty() ? project->getWindowIcon() : ios.icon;
    if (!icon.empty() && icon.is_relative()) icon = project->getProjectPath() / icon;
    settings.iconPng = icon;
    settings.runtimeDir = player->dir / "ios";
    settings.projectDir = stagingDir / getAppName();
    settings.simulator = config.iosSimulator;
    std::string error;
    std::vector<std::string> notes;
    std::vector<fs::path> artifacts;
    std::vector<BuildWarning> warnings;
    if (!platform::buildIosProject(settings, commandRunner, [this]() { return isCancelled(); }, artifacts, notes, warnings, error)) {
        setError(error);
        return false;
    }
    for (const BuildWarning& warning : warnings) warn(warning.code, warning.message);
    for (const std::string& note : notes) Out::info("%s", note.c_str());
    std::lock_guard<std::mutex> lock(progressMutex);
    result.notes = notes;
    result.artifacts = artifacts;
    result.executable = artifacts.empty() ? settings.projectDir : artifacts.front();
    return true;
}
