// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine Tools/Scripts/publish_game.py (MIT):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// (prepare/verify, vdf_string and the SteamPipe scripts, the Deck deploy's verify-then-swap),
// rewritten in C++ for our layout and tools.

#include "Publishing.h"

#include "ShippingTypes.h"
#include "io/AtomicFile.h"
#include "platform/ExternalTools.h"
#include "player/ContentManifest.h"
#include "players/PlayerRuntimes.h"
#include "publish/BinaryInfo.h"
#include "publish/PayloadValidator.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::publish;
namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    bool writeText(const fs::path& path, const std::string& text, std::string& error) {
        const io::WriteResult written = io::writeFileAtomic(path, text);
        if (!written.ok()) {
            error = io::describeWriteError(written);
            return false;
        }
        return true;
    }

    // a path inside a tree: relative, '/'-separated, no '..', '.', empty or absolute part
    bool isTreePath(const std::string& path) {
        if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
        std::stringstream parts(path);
        std::string part;
        while (std::getline(parts, part, '/'))
            if (part.empty() || part == "." || part == "..") return false;
        return path.back() != '/';
    }

    // a value safe in a remote sh command and as a remote path: no quoting tricks to get wrong
    bool isRemotePath(const std::string& path) {
        static const std::regex safe(R"(^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$)");
        return std::regex_match(path, safe) && isTreePath(path);
    }

    std::string shellQuote(const std::string& value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        return out + "'";
    }

    std::string randomSuffix() {
        std::random_device device;
        std::mt19937_64 generator((static_cast<uint64_t>(device()) << 32) ^ device() ^
                                  static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        static const char* digits = "0123456789abcdef";
        std::string out;
        uint64_t value = generator();
        for (int i = 0; i < 12; i++, value >>= 4) out += digits[value & 15];
        return out;
    }

    std::string utcNow() {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return text;
    }

    fs::path under(const fs::path& base, const std::string& value) {
        const fs::path path = fs::u8path(value);
        return (path.is_absolute() ? path : base / path).lexically_normal();
    }

    // a inside b (or the same)
    bool within(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        const fs::path ca = fs::weakly_canonical(a, ec), cb = fs::weakly_canonical(b, ec);
        auto ia = ca.begin();
        for (auto ib = cb.begin(); ib != cb.end(); ++ib, ++ia) {
            if (ib->empty()) continue;
            if (ia == ca.end() || *ia != *ib) return false;
        }
        return true;
    }

    ValidationTarget validationTarget(const std::string& platform, const std::string& arch, const std::string& backend,
                                      const std::string& executable) {
        ValidationTarget target;
        parseBuildPlatform(platform, target.platform);
        target.arch = arch;
        target.backend = backend;
        target.executable = executable;
        return target;
    }

    // the build's content-manifest.json (next to the executable, in a bundle's Resources, ...)
    bool findContentManifest(const fs::path& root, player::ContentManifest& manifest) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->path().filename() != "content-manifest.json" || !it->is_regular_file(ec)) continue;
            std::string text, error;
            if (readText(it->path(), text) && player::ContentManifest::parse(text, manifest, error)) return true;
        }
        return false;
    }

    ordered_json treeJson(const std::map<std::string, TreeEntry>& files) {
        ordered_json out = ordered_json::object();
        for (const auto& [path, entry] : files) {
            if (!entry.symlink.empty()) out[path] = {{"symlink", entry.symlink}};
            else out[path] = {{"sha256", entry.sha256}, {"size", entry.size}};
        }
        return out;
    }

    void log(const PublishTools& tools, const std::string& line) {
        if (tools.log) tools.log(line);
    }

    bool cancelled(const PublishTools& tools) {
        return tools.cancelled && tools.cancelled();
    }

    // runs a tool; its output lines to the log and `output`
    bool runTool(const PublishTools& tools, const std::vector<std::string>& args, const fs::path& workingDir, std::string& output,
                 int* exitCode = nullptr) {
        CommandRunner local;
        CommandRunner& runner = tools.runner ? *tools.runner : local;
        int code = -1;
        runner.runArgs(args, workingDir, [&](const std::string& line) {
            output += line + "\n";
            log(tools, line);
        }, &code);
        if (exitCode) *exitCode = code;
        return code == 0;
    }

    // ssh / scp argument prefixes for the Deck
    std::vector<std::string> sshArgs(const DeckSettings& deck, const fs::path& ssh) {
        std::vector<std::string> args = {ssh.string(), "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"};
        if (deck.port > 0) args.insert(args.end(), {"-p", std::to_string(deck.port)});
        if (!deck.keyFile.empty()) args.insert(args.end(), {"-i", deck.keyFile});
        args.push_back(deck.user.empty() ? deck.host : deck.user + "@" + deck.host);
        return args;
    }

    bool checkDeck(const DeckSettings& deck, std::string& error) {
        static const std::regex host(R"(^[A-Za-z0-9][A-Za-z0-9_.-]*$)");
        static const std::regex user(R"(^[A-Za-z0-9_][A-Za-z0-9_.-]*$)");
        if (!std::regex_match(deck.host, host)) {
            error = "deck.host must be a host name or IPv4 address (publishing.json), got '" + deck.host + "'";
            return false;
        }
        if (!deck.user.empty() && !std::regex_match(deck.user, user)) {
            error = "deck.user must be a user name, got '" + deck.user + "'";
            return false;
        }
        if (!isRemotePath(deck.remoteDir)) {
            error = "deck.remoteDir must be a path relative to the Deck user's home (letters, digits, '.', '_', '-', '/'), got '" +
                    deck.remoteDir + "'";
            return false;
        }
        return true;
    }

    // the lock every remote step takes, and its release
    std::string remoteLocked(const DeckSettings& deck, const std::string& body) {
        return "set -e; mkdir -p " + shellQuote(deck.remoteDir) + "; cd " + shellQuote(deck.remoteDir) +
               "; if ! mkdir .deploy.lock 2>/dev/null; then echo 'another deploy runs here (.deploy.lock)' >&2; exit 3; fi; "
               "trap 'rmdir .deploy.lock' EXIT; " + body;
    }

}

// ---------------------------------------------------------------- publishing.json

bool publish::isIdentifier(const std::string& value) {
    static const std::regex identifier(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$)");
    return std::regex_match(value, identifier);
}

bool PublishingConfig::parse(const std::string& text, PublishingConfig& out, std::string& error) {
    const ordered_json in = ordered_json::parse(text, nullptr, false);
    if (in.is_discarded() || !in.is_object()) {
        error = "publishing.json is not a JSON object";
        return false;
    }
    try {
        const int schema = in.value("schemaVersion", 0);
        if (schema > SCHEMA_VERSION) {
            error = "publishing.json is schema " + std::to_string(schema) + ", this editor (" + PlayerRuntimes::editorVersion() +
                    ") reads " + std::to_string(SCHEMA_VERSION) + ": use a newer editor";
            return false;
        }
        if (schema < 1) {
            error = "publishing.json has no schemaVersion";
            return false;
        }
        out = PublishingConfig();
        out.releasesDir = in.value("releasesDir", out.releasesDir);
        for (const auto& [name, t] : in.value("targets", ordered_json::object()).items()) {
            PublishTarget target;
            target.platform = t.value("platform", target.platform);
            target.arch = t.value("arch", target.arch);
            target.backend = t.value("backend", "");
            target.flavour = t.value("flavour", target.flavour);
            target.buildDir = t.value("buildDir", "");
            target.executable = t.value("executable", "");
            target.depotId = t.contains("depotId") ? (t["depotId"].is_string() ? t["depotId"].get<std::string>() : t["depotId"].dump()) : "";
            target.deviceProfile = t.value("deviceProfile", "");
            target.requiredFiles = t.value("requiredFiles", std::vector<std::string>{});
            out.targets[name] = target;
        }
        const ordered_json steam = in.value("steam", ordered_json::object());
        out.steam.appId = steam.contains("appId") ? (steam["appId"].is_string() ? steam["appId"].get<std::string>() : steam["appId"].dump()) : "";
        out.steam.buildDescription = steam.value("buildDescription", "");
        const ordered_json deck = in.value("deck", ordered_json::object());
        out.deck.host = deck.value("host", "");
        out.deck.user = deck.value("user", out.deck.user);
        out.deck.remoteDir = deck.value("remoteDir", out.deck.remoteDir);
        out.deck.keyFile = deck.value("keyFile", "");
        out.deck.port = deck.value("port", 0);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("publishing.json: ") + e.what();
        return false;
    }
    return true;
}

bool PublishingConfig::load(const fs::path& file, PublishingConfig& out, std::string& error) {
    std::string text;
    if (!readText(file, text)) {
        error = "no " + file.string() + " (publish init writes one)";
        return false;
    }
    return parse(text, out, error);
}

std::string PublishingConfig::toJson() const {
    ordered_json out;
    out["schemaVersion"] = SCHEMA_VERSION;
    out["releasesDir"] = releasesDir;
    ordered_json list = ordered_json::object();
    for (const auto& [name, t] : targets) {
        ordered_json target = {{"platform", t.platform}, {"arch", t.arch}, {"backend", t.backend}, {"flavour", t.flavour},
                               {"buildDir", t.buildDir}, {"executable", t.executable}};
        if (!t.depotId.empty()) target["depotId"] = t.depotId;
        if (!t.deviceProfile.empty()) target["deviceProfile"] = t.deviceProfile;
        target["requiredFiles"] = t.requiredFiles;
        list[name] = target;
    }
    out["targets"] = list;
    out["steam"] = {{"appId", steam.appId}, {"buildDescription", steam.buildDescription}};
    ordered_json deckJson = {{"host", deck.host}, {"user", deck.user}, {"remoteDir", deck.remoteDir}};
    if (!deck.keyFile.empty()) deckJson["keyFile"] = deck.keyFile;
    if (deck.port > 0) deckJson["port"] = deck.port;
    out["deck"] = deckJson;
    return out.dump(2) + "\n";
}

bool PublishingConfig::save(const fs::path& file, std::string& error) const {
    return writeText(file, toJson(), error);
}

std::vector<std::string> PublishingConfig::check() const {
    std::vector<std::string> problems;
    static const std::regex steamId(R"(^[0-9]{1,10}$)");
    if (releasesDir.empty()) problems.push_back("releasesDir is empty");
    std::set<std::string> depots;
    for (const auto& [name, t] : targets) {
        if (!isIdentifier(name)) problems.push_back("target name '" + name + "' must be letters, digits, '.', '_' or '-'");
        BuildPlatform platform;
        if (!parseBuildPlatform(t.platform, platform)) problems.push_back(name + ": unknown platform '" + t.platform + "'");
        if (t.flavour != "release" && t.flavour != "development") problems.push_back(name + ": flavour is release or development");
        if (t.buildDir.empty()) problems.push_back(name + ": buildDir is empty");
        if (!t.executable.empty() && !isTreePath(t.executable)) problems.push_back(name + ": executable must be a path inside the build");
        for (const std::string& file : t.requiredFiles)
            if (!isTreePath(file)) problems.push_back(name + ": required file '" + file + "' must be a path inside the build");
        if (!t.depotId.empty()) {
            if (!std::regex_match(t.depotId, steamId)) problems.push_back(name + ": depotId must be a number");
            else if (!depots.insert(t.depotId).second) problems.push_back(name + ": depot " + t.depotId + " is used by another target");
        }
        if (!t.deviceProfile.empty() && t.deviceProfile != "steamdeck") problems.push_back(name + ": deviceProfile is \"steamdeck\" or empty");
        if (t.deviceProfile == "steamdeck" && (t.platform != "linux" || t.arch != "x64"))
            problems.push_back(name + ": a Steam Deck target is linux x64");
    }
    if (!steam.appId.empty() && !std::regex_match(steam.appId, steamId)) problems.push_back("steam.appId must be a number");
    return problems;
}

PublishingConfig publish::defaultPublishingConfig(const std::map<std::string, std::string>& builtDirs) {
    PublishingConfig config;
    for (const auto& [platformName, dir] : builtDirs) {
        BuildPlatform platform;
        if (!parseBuildPlatform(platformName, platform)) continue;
        PublishTarget target;
        target.platform = platformName;
        target.arch = platformArch(platform);
        target.backend = platformBackends(platform).front();
        target.buildDir = dir;
        config.targets[platformName] = target;
    }
    if (config.targets.empty()) {
        // (not named linux: GCC predefines that macro)
        PublishTarget host;
        host.buildDir = "build/linux";
        host.backend = "glcore";
        config.targets["linux"] = host;
    }
    config.steam.buildDescription = "";
    return config;
}

// ---------------------------------------------------------------- release.json

std::string ReleaseInfo::toJson() const {
    ordered_json out;
    out["schemaVersion"] = SCHEMA_VERSION;
    out["release"] = release;
    out["project"] = project;
    out["engineVersion"] = engineVersion;
    out["createdUtc"] = createdUtc;
    out["steam"] = {{"appId", steamAppId}};
    ordered_json list = ordered_json::object();
    for (const auto& [name, t] : targets) {
        ordered_json target = {{"platform", t.platform}, {"arch", t.arch}, {"backend", t.backend}, {"flavour", t.flavour},
                               {"playerId", t.playerId}, {"method", t.method}, {"executable", t.executable}};
        if (!t.depotId.empty()) target["depotId"] = t.depotId;
        if (!t.deviceProfile.empty()) target["deviceProfile"] = t.deviceProfile;
        if (!t.glibcFloor.empty()) target["glibcFloor"] = t.glibcFloor;
        target["files"] = treeJson(t.files);
        list[name] = target;
    }
    out["targets"] = list;
    return out.dump(2) + "\n";
}

bool ReleaseInfo::load(const fs::path& releaseDir, ReleaseInfo& out, std::string& error) {
    std::string text;
    if (!readText(releaseDir / "release.json", text)) {
        error = "no release.json in " + releaseDir.string();
        return false;
    }
    const ordered_json in = ordered_json::parse(text, nullptr, false);
    if (in.is_discarded() || !in.is_object()) {
        error = "release.json is not a JSON object";
        return false;
    }
    try {
        const int schema = in.value("schemaVersion", 0);
        if (schema > SCHEMA_VERSION) {
            error = "release.json is schema " + std::to_string(schema) + ", this editor (" + PlayerRuntimes::editorVersion() + ") reads " +
                    std::to_string(SCHEMA_VERSION) + ": use a newer editor";
            return false;
        }
        if (schema < 1 || !in.contains("targets") || in["targets"].empty()) {
            error = "release.json is not a release manifest (no schemaVersion or targets)";
            return false;
        }
        out = ReleaseInfo();
        out.release = in.value("release", "");
        out.project = in.value("project", "");
        out.engineVersion = in.value("engineVersion", "");
        out.createdUtc = in.value("createdUtc", "");
        out.steamAppId = in.value("steam", ordered_json::object()).value("appId", "");
        for (const auto& [name, t] : in["targets"].items()) {
            ReleaseTarget target;
            target.platform = t.value("platform", "");
            target.arch = t.value("arch", "");
            target.backend = t.value("backend", "");
            target.flavour = t.value("flavour", "");
            target.playerId = t.value("playerId", "");
            target.method = t.value("method", "");
            target.executable = t.value("executable", "");
            target.depotId = t.value("depotId", "");
            target.deviceProfile = t.value("deviceProfile", "");
            target.glibcFloor = t.value("glibcFloor", "");
            for (const auto& [path, f] : t.value("files", ordered_json::object()).items()) {
                TreeEntry entry;
                if (f.contains("symlink")) entry.symlink = f["symlink"].get<std::string>();
                else {
                    entry.sha256 = f.value("sha256", "");
                    entry.size = f.value("size", uint64_t(0));
                }
                target.files[path] = entry;
            }
            out.targets[name] = target;
        }
    } catch (const nlohmann::json::exception& e) {
        error = std::string("release.json: ") + e.what();
        return false;
    }
    return true;
}

std::string publish::treeGlibcFloor(const fs::path& root) {
    std::string floor;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_symlink(ec) || !it->is_regular_file(ec)) continue;
        std::string error;
        const std::optional<BinaryInfo> info = inspectBinary(it->path(), &error);
        if (info && info->format == BinaryInfo::Elf && !info->glibcFloor.empty() && (floor.empty() || glibcNewer(info->glibcFloor, floor)))
            floor = info->glibcFloor;
    }
    return floor;
}

// ---------------------------------------------------------------- prepare / verify

namespace {

    // moves a build's debug information to symbols/<target>/ (R23)
    bool splitSymbols(const fs::path& content, const fs::path& symbols, const PublishTarget& target, const PublishTools& tools,
                      std::vector<std::string>& warnings, std::vector<std::string>& problems) {
        std::error_code ec;
        std::vector<fs::path> pdbs, dsyms, elves;
        for (fs::recursive_directory_iterator it(content, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path& path = it->path();
            if (it->is_symlink(ec)) continue;
            if (it->is_directory(ec) && path.extension() == ".dSYM") {
                dsyms.push_back(path);
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".pdb" || ext == ".debug") {
                pdbs.push_back(path);
                continue;
            }
            if (target.platform == "linux" || target.platform == "android") {
                std::string error;
                const std::optional<BinaryInfo> info = inspectBinary(path, &error);
                if (info && info->format == BinaryInfo::Elf && info->hasDebugInfo) elves.push_back(path);
            }
        }
        auto moveOut = [&](const fs::path& path) {
            const fs::path to = symbols / path.lexically_relative(content);
            fs::create_directories(to.parent_path(), ec);
            fs::rename(path, to, ec);
            if (ec) {
                fs::copy(path, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (!ec) fs::remove_all(path, ec);
            }
            if (ec) problems.push_back("cannot move " + path.string() + " to the symbols: " + ec.message());
        };
        // a macOS bundle's .dSYM leaves before anything signs it again
        for (const fs::path& path : dsyms) moveOut(path);
        for (const fs::path& path : pdbs) moveOut(path);
        if (elves.empty()) return problems.empty();
        const platform::ToolLocation objcopy = platform::ExternalTools::objcopy();
        const platform::ToolLocation strip = platform::ExternalTools::strip();
        if (!objcopy.found() || !strip.found()) {
            std::string list;
            for (const fs::path& path : elves) list += (list.empty() ? "" : ", ") + path.lexically_relative(content).generic_string();
            warnings.push_back("W-SYMBOLS-NOT-SPLIT: no objcopy/strip (binutils), the debug information stays in " + list);
            return problems.empty();
        }
        for (const fs::path& path : elves) {
            const fs::path debug = symbols / (path.lexically_relative(content).string() + ".debug");
            fs::create_directories(debug.parent_path(), ec);
            std::string output;
            if (!runTool(tools, {objcopy.path.string(), "--only-keep-debug", path.string(), debug.string()}, content, output) ||
                !runTool(tools, {strip.path.string(), "--strip-debug", path.string()}, content, output) ||
                !runTool(tools, {objcopy.path.string(), "--add-gnu-debuglink=" + debug.string(), path.string()}, content, output)) {
                problems.push_back("splitting the symbols of " + path.lexically_relative(content).generic_string() + " failed: " + output);
            }
        }
        return problems.empty();
    }

    // a prebuilt player's own symbols (its runtime's symbols/, when the archive shipped them)
    void copyPlayerSymbols(const std::string& playerId, const fs::path& symbols, std::vector<std::string>& warnings) {
        if (playerId.empty()) return;
        std::string error;
        const std::optional<PlayerInfo> info = PlayerRuntimes::find(playerId, {}, true, error);
        std::error_code ec;
        if (!info || !fs::is_directory(info->dir / "symbols", ec)) {
            warnings.push_back("W-PLAYER-SYMBOLS: player " + playerId + " has no symbols installed (jasperengine-player-" + playerId + "-" +
                               PlayerRuntimes::editorVersion() + "-symbols.zip): its crashes symbolicate from the release's archive");
            return;
        }
        fs::create_directories(symbols / "player", ec);
        fs::copy(info->dir / "symbols", symbols / "player", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) warnings.push_back("W-PLAYER-SYMBOLS: cannot copy the symbols of player " + playerId + ": " + ec.message());
    }

}

bool publish::prepareRelease(const PrepareRequest& request, const PublishTools& tools, PrepareResult& result, std::vector<std::string>& problems) {
    if (!isIdentifier(request.release)) {
        problems.push_back("release name '" + request.release + "' must be letters, digits, '.', '_' or '-' (e.g. 1.0.0)");
        return false;
    }
    for (const std::string& problem : request.config.check()) problems.push_back("publishing.json: " + problem);
    if (!problems.empty()) return false;

    std::map<std::string, PublishTarget> targets;
    if (request.targets.empty()) {
        targets = request.config.targets;
    } else {
        for (const std::string& name : request.targets) {
            auto it = request.config.targets.find(name);
            if (it == request.config.targets.end()) problems.push_back("no target '" + name + "' in publishing.json");
            else targets[name] = it->second;
        }
    }
    if (targets.empty() && problems.empty()) problems.push_back("publishing.json has no targets");
    if (!problems.empty()) return false;

    const fs::path releases = under(request.projectDir, request.config.releasesDir);
    const fs::path final = releases / request.release;
    std::error_code ec;
    if (fs::exists(final, ec)) {
        problems.push_back("release " + request.release + " exists already (" + final.string() + "): choose a new release name");
        return false;
    }
    for (const auto& [name, target] : targets) {
        const fs::path source = under(request.projectDir, target.buildDir);
        if (!fs::is_directory(source, ec)) {
            problems.push_back(name + ": no build in " + source.string() + " (build it first)");
            continue;
        }
        if (within(releases, source) || within(source, releases)) {
            problems.push_back(name + ": the release storage " + releases.string() + " and the build " + source.string() + " overlap");
            continue;
        }
        std::vector<std::string> findings;
        validatePayload(source, validationTarget(target.platform, target.arch, target.backend, target.executable), findings);
        for (const std::string& file : target.requiredFiles)
            if (!fs::exists(source / fs::u8path(file), ec)) findings.push_back("missing required file " + file);
        for (const std::string& finding : findings) problems.push_back(name + ": " + finding);
    }
    if (!problems.empty()) return false;

    fs::create_directories(releases, ec);
    const fs::path staging = releases / (".preparing-" + randomSuffix());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            if (!path.empty()) fs::remove_all(path, ec);
        }
    } cleanup{staging};

    ReleaseInfo info;
    info.release = request.release;
    info.project = request.projectName;
    info.engineVersion = PlayerRuntimes::editorVersion();
    info.createdUtc = utcNow();
    info.steamAppId = request.config.steam.appId;
    for (const auto& [name, target] : targets) {
        if (cancelled(tools)) {
            problems.push_back("cancelled");
            return false;
        }
        log(tools, "Preparing " + name + "...");
        const fs::path source = under(request.projectDir, target.buildDir);
        const fs::path content = staging / "content" / name;
        const fs::path symbols = staging / "symbols" / name;
        fs::create_directories(content.parent_path(), ec);
        fs::copy(source, content, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            problems.push_back(name + ": cannot copy " + source.string() + ": " + ec.message());
            return false;
        }
        if (!splitSymbols(content, symbols, target, tools, result.warnings, problems)) return false;

        player::ContentManifest manifest;
        const bool hasManifest = findContentManifest(content, manifest);
        if (hasManifest && manifest.method == "prebuilt") copyPlayerSymbols(manifest.playerId, symbols, result.warnings);

        // the payload after the split is the one that ships
        std::vector<std::string> findings;
        validatePayload(content, validationTarget(target.platform, target.arch, target.backend, target.executable), findings);
        for (const std::string& finding : findings) problems.push_back(name + ": " + finding);
        TreeManifestResult tree = treeManifest(content, targetOs(validationTarget(target.platform, target.arch, target.backend, "").platform));
        for (const std::string& problem : tree.problems) problems.push_back(name + ": " + problem);
        if (!problems.empty()) return false;

        ReleaseTarget entry;
        entry.platform = target.platform;
        entry.arch = target.arch;
        entry.backend = target.backend.empty() && hasManifest ? manifest.backend : target.backend;
        entry.flavour = target.flavour;
        entry.playerId = hasManifest ? manifest.playerId : "";
        entry.method = hasManifest ? manifest.method : "";
        entry.executable = target.executable;
        entry.depotId = target.depotId;
        entry.deviceProfile = target.deviceProfile;
        if (target.platform == "linux") entry.glibcFloor = treeGlibcFloor(content);
        entry.files = std::move(tree.entries);
        info.targets[name] = std::move(entry);
    }
    std::string error;
    if (!writeText(staging / "release.json", info.toJson(), error)) {
        problems.push_back("release.json: " + error);
        return false;
    }
    if (!io::renameWithRetry(staging, final, ec)) {
        problems.push_back("cannot move the release into " + final.string() + ": " + ec.message());
        return false;
    }
    cleanup.path.clear();
    result.releaseDir = final;
    return true;
}

bool publish::verifyRelease(const fs::path& releaseDir, std::vector<std::string>& problems, ReleaseInfo* infoOut) {
    ReleaseInfo info;
    std::string error;
    if (!ReleaseInfo::load(releaseDir, info, error)) {
        problems.push_back(error);
        return false;
    }
    for (const auto& [name, target] : info.targets) {
        if (!isIdentifier(name)) {
            problems.push_back("release.json names a target '" + name + "'");
            continue;
        }
        const fs::path content = releaseDir / "content" / name;
        std::vector<std::string> findings;
        const ValidationTarget validation = validationTarget(target.platform, target.arch, target.backend, target.executable);
        validatePayload(content, validation, findings);
        for (const std::string& finding : findings) problems.push_back(name + ": " + finding);
        const TreeManifestResult tree = treeManifest(content, targetOs(validation.platform));
        for (const std::string& problem : tree.problems) problems.push_back(name + ": " + problem);
        std::set<std::string> paths;
        for (const auto& [path, entry] : tree.entries) paths.insert(path);
        for (const auto& [path, entry] : target.files) paths.insert(path);
        for (const std::string& path : paths) {
            auto now = tree.entries.find(path);
            auto then = target.files.find(path);
            const bool same = now != tree.entries.end() && then != target.files.end() && now->second.sha256 == then->second.sha256 &&
                              now->second.size == then->second.size && now->second.symlink == then->second.symlink;
            if (!same) problems.push_back("Release content changed after preparation: " + name + "/" + path);
        }
    }
    if (infoOut) *infoOut = info;
    return problems.empty();
}

// ---------------------------------------------------------------- Steam

std::string publish::vdfString(const std::string& value, std::string& error) {
    for (char c : value) {
        if (static_cast<unsigned char>(c) < 32) {
            error = "SteamPipe values cannot hold control characters";
            return "";
        }
    }
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool publish::writeSteamScripts(const fs::path& releaseDir, bool preview, const std::string& branch, fs::path& appScript,
                                std::vector<fs::path>& depotScripts, std::string& error) {
    std::vector<std::string> problems;
    ReleaseInfo info;
    if (!verifyRelease(releaseDir, problems, &info)) {
        error = problems.front();
        for (size_t i = 1; i < problems.size(); i++) error += "\n" + problems[i];
        return false;
    }
    static const std::regex steamId(R"(^[0-9]{1,10}$)");
    if (!std::regex_match(info.steamAppId, steamId)) {
        error = "the release has no Steam app id (publishing.json steam.appId, then prepare a new release)";
        return false;
    }
    std::string lower = branch;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "default" || lower == "public") {
        error = "SetLive on '" + branch + "' is refused: promote public releases in Steamworks (automatic SetLive is for beta branches)";
        return false;
    }
    if (!branch.empty() && !isIdentifier(branch)) {
        error = "branch '" + branch + "' must be letters, digits, '.', '_' or '-'";
        return false;
    }
    const fs::path scripts = releaseDir / "steampipe";
    std::error_code ec;
    fs::create_directories(scripts, ec);
    depotScripts.clear();
    std::set<std::string> depots;
    std::string depotList;
    for (const auto& [name, target] : info.targets) {
        if (!std::regex_match(target.depotId, steamId)) {
            error = "target " + name + " has no depotId (publishing.json)";
            return false;
        }
        if (!depots.insert(target.depotId).second) {
            error = "targets need distinct depot ids (" + target.depotId + " is used twice): a Deck plays the Linux depot";
            return false;
        }
        const std::string root = vdfString(fs::absolute(releaseDir / "content" / name).generic_string(), error);
        if (!error.empty()) return false;
        const std::string text = "\"DepotBuild\"\n{\n  \"DepotID\" \"" + target.depotId + "\"\n  \"ContentRoot\" " + root +
                                 "\n  \"FileMapping\"\n  {\n    \"LocalPath\" \"*\"\n    \"DepotPath\" \".\"\n    \"recursive\" \"1\"\n  }\n}\n";
        const fs::path file = scripts / ("depot_build_" + target.depotId + ".vdf");
        if (!writeText(file, text, error)) return false;
        depotScripts.push_back(file);
        depotList += "    \"" + target.depotId + "\" \"depot_build_" + target.depotId + ".vdf\"\n";
    }
    const fs::path cache = fs::absolute(releaseDir.parent_path() / ".steampipe-cache" / info.steamAppId);
    fs::create_directories(cache, ec);
    const std::string description = vdfString(info.release, error);
    const std::string output = vdfString(cache.generic_string(), error);
    if (!error.empty()) return false;
    std::string app = "\"AppBuild\"\n{\n  \"AppID\" \"" + info.steamAppId + "\"\n  \"Desc\" " + description + "\n  \"BuildOutput\" " + output +
                      "\n  \"Preview\" \"" + (preview ? "1" : "0") + "\"\n";
    if (!branch.empty() && !preview) app += "  \"SetLive\" " + vdfString(branch, error) + "\n";
    app += "  \"Depots\"\n  {\n" + depotList + "  }\n}\n";
    appScript = scripts / ("app_build_" + info.steamAppId + ".vdf");
    return writeText(appScript, app, error);
}

bool publish::steamUpload(const fs::path& releaseDir, const std::string& user, const std::string& branch, const fs::path& steamcmd,
                          const PublishTools& tools, std::string& error) {
    if (steamcmd.empty()) {
        error = "steamcmd was not found (Preferences > External Tools, STEAMCMD or PATH): install SteamCMD";
        return false;
    }
    if (user.empty()) {
        error = "the Steam build account is needed (--user or STEAM_BUILD_USER); SteamCMD logs in with its own cached session";
        return false;
    }
    static const std::regex account(R"(^[A-Za-z0-9_]{1,64}$)");
    if (!std::regex_match(user, account)) {
        error = "'" + user + "' is not a Steam account name";
        return false;
    }
    fs::path app;
    std::vector<fs::path> depots;
    if (!writeSteamScripts(releaseDir, false, branch, app, depots, error)) return false;
    std::string output;
    int code = -1;
    log(tools, "Uploading " + releaseDir.filename().string() + " with SteamCMD...");
    if (!runTool(tools, {steamcmd.string(), "+login", user, "+run_app_build", fs::absolute(app).string(), "+quit"}, releaseDir, output, &code)) {
        error = "steamcmd exited with " + std::to_string(code) + " (log in once interactively: steamcmd +login " + user + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Steam Deck

bool publish::deckDeploy(const fs::path& releaseDir, const std::string& targetName, const DeckSettings& deck, const PublishTools& tools,
                         std::string& output, std::string& error) {
    if (!checkDeck(deck, error)) return false;
    std::vector<std::string> problems;
    ReleaseInfo info;
    if (!verifyRelease(releaseDir, problems, &info)) {
        error = problems.front();
        return false;
    }
    auto it = info.targets.find(targetName);
    if (it == info.targets.end()) {
        error = "the release has no target '" + targetName + "'";
        return false;
    }
    const ReleaseTarget& target = it->second;
    if (target.platform != "linux" || target.arch != "x64") {
        error = "a Steam Deck plays a linux x64 target; " + targetName + " is " + target.platform + " " + target.arch;
        return false;
    }
    if (!isRemotePath(target.executable)) {
        error = "the target's executable '" + target.executable + "' is not a plain relative path";
        return false;
    }
    const platform::ToolLocation ssh = platform::ExternalTools::ssh();
    const platform::ToolLocation scp = platform::ExternalTools::scp();
    if (!ssh.found() || !scp.found()) {
        error = "ssh and scp (OpenSSH) are needed for a Deck deploy: " + (ssh.found() ? scp.hint : ssh.hint);
        return false;
    }
    // the checksums the Deck checks after the copy (sha256sum -c), symlinks by existence
    std::string sums;
    std::string links;
    for (const auto& [path, entry] : target.files) {
        if (path.find('\n') != std::string::npos || path.find('\'') != std::string::npos) {
            error = "'" + path + "' cannot be deployed over ssh (a newline or quote in its name)";
            return false;
        }
        if (entry.symlink.empty()) sums += entry.sha256 + "  " + path + "\n";
        else links += "test -e " + shellQuote(path) + "; ";
    }
    const fs::path local = releaseDir / "content" / targetName;
    const std::string incoming = info.release + "-" + targetName + ".incoming";
    const fs::path temp = fs::temp_directory_path() / ("jasperengine-deck-" + randomSuffix());
    std::error_code ec;
    fs::create_directories(temp, ec);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{temp};
    if (!writeText(temp / ".deploy-sha256", sums, error)) return false;

    const std::string remote = deck.remoteDir + "/" + incoming;
    std::vector<std::string> args = sshArgs(deck, ssh.path);
    args.push_back(remoteLocked(deck, "rm -rf " + shellQuote(incoming)));
    log(tools, "Deck: preparing " + deck.host + ":" + remote);
    if (!runTool(tools, args, releaseDir, output)) {
        error = "ssh " + deck.host + " failed (is the Deck reachable, with ssh enabled and your key in ~/.ssh/authorized_keys?):\n" + output;
        return false;
    }
    std::vector<std::string> copy = {scp.path.string(), "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-r", "-p", "-q"};
    if (deck.port > 0) copy.insert(copy.end(), {"-P", std::to_string(deck.port)});
    if (!deck.keyFile.empty()) copy.insert(copy.end(), {"-i", deck.keyFile});
    const std::string host = deck.user.empty() ? deck.host : deck.user + "@" + deck.host;
    copy.push_back(local.string());
    copy.push_back(host + ":" + remote);
    log(tools, "Deck: copying " + std::to_string(target.files.size()) + " files...");
    if (!runTool(tools, copy, releaseDir, output)) {
        error = "scp to " + deck.host + " failed:\n" + output;
        return false;
    }
    std::vector<std::string> manifest = {scp.path.string(), "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-q"};
    if (deck.port > 0) manifest.insert(manifest.end(), {"-P", std::to_string(deck.port)});
    if (!deck.keyFile.empty()) manifest.insert(manifest.end(), {"-i", deck.keyFile});
    manifest.push_back((temp / ".deploy-sha256").string());
    manifest.push_back(host + ":" + remote + "/.deploy-sha256");
    if (!runTool(tools, manifest, releaseDir, output)) {
        error = "scp of the checksums to " + deck.host + " failed:\n" + output;
        return false;
    }
    // checked there, then swapped in; exactly one previous build stays
    const std::string swap =
        "cd " + shellQuote(incoming) + "; sha256sum -c --quiet --strict .deploy-sha256 || { echo 'the copy on the Deck does not match the release' >&2; exit 4; }; " +
        links + "rm -f .deploy-sha256; chmod +x " + shellQuote(target.executable) + "; printf '%s\\n' " + shellQuote(info.release) +
        " > .jasper-release; cd ..; rm -rf previous; if [ -d current ]; then mv current previous; fi; mv " + shellQuote(incoming) +
        " current; echo \"deployed: " + info.release + " (" + targetName + ")\"";
    args = sshArgs(deck, ssh.path);
    args.push_back(remoteLocked(deck, swap));
    if (!runTool(tools, args, releaseDir, output)) {
        error = "installing the build on " + deck.host + " failed:\n" + output;
        return false;
    }
    return true;
}

bool publish::deckRollback(const DeckSettings& deck, const PublishTools& tools, std::string& output, std::string& error) {
    if (!checkDeck(deck, error)) return false;
    const platform::ToolLocation ssh = platform::ExternalTools::ssh();
    if (!ssh.found()) {
        error = "ssh (OpenSSH) is needed: " + ssh.hint;
        return false;
    }
    std::vector<std::string> args = sshArgs(deck, ssh.path);
    args.push_back(remoteLocked(deck,
        "if [ ! -d previous ]; then echo 'no previous build to restore' >&2; exit 4; fi; rm -rf .rollback; "
        "if [ -d current ]; then mv current .rollback; fi; mv previous current; if [ -d .rollback ]; then mv .rollback previous; fi; "
        "echo \"restored: $(cat current/.jasper-release 2>/dev/null)\""));
    if (!runTool(tools, args, {}, output)) {
        error = "rollback on " + deck.host + " failed:\n" + output;
        return false;
    }
    return true;
}

bool publish::deckSmoke(const DeckSettings& deck, const std::string& executable, const PublishTools& tools, int& exitCode,
                        std::string& output, std::string& error) {
    if (!checkDeck(deck, error)) return false;
    if (!isRemotePath(executable)) {
        error = "the executable '" + executable + "' is not a plain relative path";
        return false;
    }
    const platform::ToolLocation ssh = platform::ExternalTools::ssh();
    if (!ssh.found()) {
        error = "ssh (OpenSSH) is needed: " + ssh.hint;
        return false;
    }
    std::vector<std::string> args = sshArgs(deck, ssh.path);
    // the player's own exit code comes back (R19); the log's tail is the output
    args.push_back("cd " + shellQuote(deck.remoteDir + "/current") + " || exit 4; export DISPLAY=\"${DISPLAY:-:0}\"; ./" +
                   shellQuote(executable) + " --quit-after 600 --fail-on-error > .jasper-smoke.log 2>&1; code=$?; tail -n 60 .jasper-smoke.log; exit $code");
    runTool(tools, args, {}, output, &exitCode);
    if (exitCode == 255) {
        error = "ssh " + deck.host + " failed:\n" + output;
        return false;
    }
    return true;
}
