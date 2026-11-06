// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerRuntimes.h"
#include "PlayerRuntimeHashes.gen.h"

#include "App.h"
#include "EditorVersion.h"
#include "htmlui/HttpClient.h"
#include "io/AtomicFile.h"
#include "serialization/CookedScene.h"
#include "util/FileUtils.h"
#include "util/Sha256.h"
#include "util/ZipArchive.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>

#include <thread>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    bool renameRetry(const fs::path& from, const fs::path& to) {
        std::error_code error;
        return io::renameWithRetry(from, to, error);
    }

    uint64_t directorySize(const fs::path& dir) {
        uint64_t total = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) total += it->file_size(ec);
        return total;
    }

    std::string majorOf(const std::string& version) {
        return version.substr(0, version.find('.'));
    }

}

bool PlayerInfo::fromJson(const std::string& text, PlayerInfo& out, std::string& error) {
    const json in = json::parse(text, nullptr, false);
    if (in.is_discarded() || !in.is_object()) {
        error = "player-info.json is not valid JSON";
        return false;
    }
    try {
        const int schema = in.value("schemaVersion", 0);
        if (schema < 1 || schema > SCHEMA_VERSION) {
            error = "player-info.json is schema " + std::to_string(schema) + ", this editor reads " + std::to_string(SCHEMA_VERSION) +
                    " (use the editor of the player's version)";
            return false;
        }
        PlayerInfo info;
        info.id = in.at("id").get<std::string>();
        info.platform = in.value("platform", "");
        info.arch = in.value("arch", "");
        info.backend = in.value("backend", "");
        info.flavour = in.value("flavour", "");
        info.engineVersion = in.value("engineVersion", "");
        info.gitSha = in.value("gitSha", "");
        info.contentFormatVersion = in.value("contentFormatVersion", "");
        info.modelCookFormats = in.value("modelCookFormats", std::vector<int>{});
        info.capacities = in.value("capacities", std::map<std::string, unsigned int>{});
        info.features = in.value("features", std::vector<std::string>{});
        info.variant = in.value("variant", std::string("full"));
        info.notices = in.value("notices", std::vector<std::string>{});
        info.executables = in.value("executables", std::map<std::string, std::string>{});
        info.executable = in.value("executable", "");
        out = std::move(info);
        return true;
    } catch (const json::exception& e) {
        error = std::string("player-info.json: ") + e.what();
        return false;
    }
}

std::string PlayerInfo::toJson() const {
    json out;
    out["schemaVersion"] = SCHEMA_VERSION;
    out["id"] = id;
    out["platform"] = platform;
    out["arch"] = arch;
    out["backend"] = backend;
    out["flavour"] = flavour;
    out["engineVersion"] = engineVersion;
    out["gitSha"] = gitSha;
    out["contentFormatVersion"] = contentFormatVersion;
    out["modelCookFormats"] = modelCookFormats;
    if (!capacities.empty()) out["capacities"] = capacities;
    out["features"] = features;
    out["variant"] = variant;
    out["notices"] = notices;
    out["executables"] = executables;
    out["executable"] = executable;
    return out.dump(2) + "\n";
}

bool PlayerInfo::load(const fs::path& dir, PlayerInfo& out, std::string& error) {
    std::ifstream in(dir / "player-info.json", std::ios::binary);
    if (!in) {
        error = "no player-info.json in " + dir.string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!fromJson(text, out, error)) return false;
    out.dir = dir;
    out.sizeBytes = directorySize(dir);
    for (const auto& [binary, sha] : out.executables) {
        std::error_code ec;
        const uint64_t size = fs::file_size(dir / binary, ec);
        if (!ec) out.shippedBytes += size;
    }
    return true;
}

bool PlayerInfo::hasModules(const std::vector<std::string>& modules) const {
    for (const std::string& module : modules)
        if (std::find(features.begin(), features.end(), module) == features.end()) return false;
    return true;
}

std::string PlayerRuntimes::runtimeId(const std::string& platform, const std::string& arch, const std::string& backend, bool development,
                                      const std::string& variantSuffix) {
    return platform + "-" + arch + "-" + backend + "-" + (development ? "development" : "release") +
           (variantSuffix.empty() ? std::string() : "-" + variantSuffix);
}

const std::vector<PlayerVariant>& PlayerRuntimes::variants() {
    static const std::vector<PlayerVariant> list = []() {
        std::vector<PlayerVariant> out;
        std::ifstream in(FileUtils::getEngineDir() / "player" / "variants.json", std::ios::binary);
        const json parsed = json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), nullptr, false);
        if (parsed.is_object() && parsed.contains("variants") && parsed["variants"].is_array()) {
            for (const json& variant : parsed["variants"]) {
                if (!variant.is_object()) continue;
                out.push_back({variant.value("name", std::string()), variant.value("suffix", std::string()),
                               variant.value("modules", std::vector<std::string>{})});
            }
        }
        std::stable_partition(out.begin(), out.end(), [](const PlayerVariant& v) { return v.suffix.empty(); });
        if (out.empty() || !out.front().suffix.empty())
            out.insert(out.begin(), PlayerVariant{"full", "", {"htmlui", "video", "runtimeshaders", "physics3d", "physics2d", "navigation", "soundlayer"}});
        return out;
    }();
    return list;
}

const PlayerVariant& PlayerRuntimes::variantFor(const std::vector<std::string>& modules) {
    const PlayerVariant* best = &variants().front();
    for (const PlayerVariant& variant : variants()) {
        const bool holds = std::all_of(modules.begin(), modules.end(), [&](const std::string& m) {
            return std::find(variant.modules.begin(), variant.modules.end(), m) != variant.modules.end();
        });
        // (a tie goes to the later one: variants.json lists them from the largest to the smallest)
        if (holds && variant.modules.size() <= best->modules.size()) best = &variant;
    }
    return *best;
}

std::vector<PlayerInfo> PlayerRuntimes::findVariants(const std::string& baseId, const fs::path& override, bool allowMismatch) {
    std::vector<PlayerInfo> out;
    for (const PlayerVariant& variant : variants()) {
        std::string error;
        std::optional<PlayerInfo> found = find(variant.suffix.empty() ? baseId : baseId + "-" + variant.suffix, override, allowMismatch, error);
        if (found) out.push_back(std::move(*found));
    }
    return out;
}

const PlayerInfo* PlayerRuntimes::smallestWith(const std::vector<PlayerInfo>& candidates, const std::vector<std::string>& modules) {
    const PlayerInfo* best = nullptr;
    for (const PlayerInfo& candidate : candidates) {
        if (!candidate.hasModules(modules)) continue;
        if (!best || candidate.shippedBytes < best->shippedBytes ||
            (candidate.shippedBytes == best->shippedBytes && candidate.features.size() < best->features.size()))
            best = &candidate;
    }
    return best;
}

std::string PlayerRuntimes::editorVersion() {
    return JASPERENGINE_EDITOR_VERSION;
}

bool PlayerRuntimes::isReleaseEditor() {
    static const std::regex release(R"(^v[0-9]+\.[0-9]+\.[0-9]+$)");
    return std::regex_match(editorVersion(), release);
}

std::vector<fs::path> PlayerRuntimes::searchDirs(const fs::path& override) {
    std::vector<fs::path> dirs;
    if (!override.empty()) dirs.push_back(override);
    if (const char* env = std::getenv("JASPER_PLAYERS_DIR"); env && *env) dirs.push_back(env);
    // the editor install's own (bin/../players, or players/ next to a portable editor)
    const fs::path exe = FileUtils::getExecutableDir();
    dirs.push_back(exe.parent_path() / "players");
    dirs.push_back(exe / "players");
    dirs.push_back(installRoot());
    return dirs;
}

fs::path PlayerRuntimes::installRoot(const fs::path& override) {
    if (!override.empty()) return override;
    return App::getUserCacheBaseDir() / "jasperengine" / "players" / editorVersion();
}

std::vector<PlayerInfo> PlayerRuntimes::list(const fs::path& override) {
    std::vector<PlayerInfo> out;
    std::set<std::string> seen;
    for (const fs::path& dir : searchDirs(override)) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            PlayerInfo info;
            std::string error;
            if (!PlayerInfo::load(entry.path(), info, error)) continue;
            if (seen.insert(info.id).second) out.push_back(std::move(info));
        }
    }
    std::sort(out.begin(), out.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
    return out;
}

std::optional<PlayerInfo> PlayerRuntimes::find(const std::string& id, const fs::path& override, bool allowMismatch, std::string& error) {
    std::vector<std::string> rejected;
    for (const PlayerInfo& info : list(override)) {
        if (info.id != id) continue;
        if (!allowMismatch && info.engineVersion != editorVersion()) {
            rejected.push_back(info.dir.string() + " is for editor " + info.engineVersion);
            continue;
        }
        if (majorOf(info.contentFormatVersion) != majorOf(jasperengine::cook::contentFormatVersion())) {
            rejected.push_back(info.dir.string() + " reads content format " + info.contentFormatVersion);
            continue;
        }
        return info;
    }
    error = "no player runtime " + id + " for editor " + editorVersion();
    for (const std::string& why : rejected) error += "; " + why;
    error += isReleaseEditor() ? " (Build Settings > Download, or `jasperengine-editor players download " + id + "`)"
                               : " (a development editor uses local players: tools/build.sh players, then --players-dir or JASPER_PLAYERS_DIR)";
    return std::nullopt;
}

const std::vector<PlayerArchiveHash>& PlayerRuntimes::releaseHashes() {
    static const std::vector<PlayerArchiveHash> hashes = {JASPERENGINE_PLAYER_RUNTIME_HASHES};
    return hashes;
}

bool PlayerRuntimes::install(const fs::path& archive, const fs::path& override, std::string& installedId, std::vector<std::string>& warnings,
                             std::string& error, const std::vector<PlayerArchiveHash>* hashes) {
    const std::vector<PlayerArchiveHash>& known = hashes ? *hashes : releaseHashes();
    std::string archiveHash;
    uint64_t archiveSize = 0;
    if (!sha256File(archive, archiveHash, &error, &archiveSize)) return false;

    ZipReader zip;
    if (!zip.open(archive, error)) return false;
    std::vector<uint8_t> infoBytes;
    if (!zip.read("player-info.json", infoBytes, error)) {
        error = archive.filename().string() + " is not a player runtime archive: " + error;
        return false;
    }
    PlayerInfo info;
    if (!PlayerInfo::fromJson(std::string(infoBytes.begin(), infoBytes.end()), info, error)) return false;

    if (!known.empty()) {
        const PlayerArchiveHash* expected = nullptr;
        for (const PlayerArchiveHash& hash : known)
            if (info.id == hash.id) expected = &hash;
        if (!expected) {
            error = "player " + info.id + " is not one of editor " + editorVersion() + "'s runtimes";
            return false;
        }
        if (archiveHash != expected->sha256) {
            error = archive.filename().string() + " is not the release's archive of " + info.id + " (SHA-256 " + archiveHash +
                    ", expected " + expected->sha256 + "): download it again";
            return false;
        }
    } else {
        warnings.push_back("this development editor has no release hashes: " + archive.filename().string() +
                           " is checked against its own player-info.json only");
    }

    const fs::path root = installRoot(override);
    const fs::path target = root / info.id;
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path temp = root / (".installing-" + info.id + "-" + std::to_string(io::currentProcessId()));
    fs::remove_all(temp, ec);
    auto cleanup = [&]() { fs::remove_all(temp, ec); };
    if (!zip.extractAll(temp, error)) {
        cleanup();
        return false;
    }
    for (const auto& [relative, sha] : info.executables) {
        std::string hex;
        std::string why;
        if (!sha256File(temp / editor::FileUtils::pathFromUtf8(relative), hex, &why)) {
            cleanup();
            error = "player " + info.id + ": " + why;
            return false;
        }
        if (hex != sha) {
            cleanup();
            error = "player " + info.id + ": " + relative + " does not match its player-info.json SHA-256 (a damaged archive)";
            return false;
        }
        fs::permissions(temp / editor::FileUtils::pathFromUtf8(relative), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
    }
    // the old one aside first, so the swap never leaves no runtime
    const fs::path previous = root / (".previous-" + info.id + "-" + std::to_string(io::currentProcessId()));
    if (fs::exists(target, ec) && !renameRetry(target, previous)) {
        cleanup();
        error = "cannot replace " + target.string();
        return false;
    }
    if (!renameRetry(temp, target)) {
        if (fs::exists(previous, ec)) renameRetry(previous, target);
        cleanup();
        error = "cannot install into " + target.string();
        return false;
    }
    fs::remove_all(previous, ec);
    installedId = info.id;
    return true;
}

bool PlayerRuntimes::remove(const std::string& id, const fs::path& override, std::string& error) {
    const fs::path target = installRoot(override) / id;
    std::error_code ec;
    if (!fs::is_directory(target, ec)) {
        error = "no installed player " + id + " in " + installRoot(override).string() +
                " (players next to the editor are part of its install and stay)";
        return false;
    }
    fs::remove_all(target, ec);
    if (ec) {
        error = "cannot remove " + target.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string PlayerRuntimes::downloadUrl(const std::string& id) {
    const std::string version = editorVersion();
    std::string base = "https://github.com/JasperRaccoon/JasperEngine/releases/download/";
    if (const char* mirror = std::getenv("JASPER_PLAYERS_URL"); mirror && *mirror) base = std::string(mirror) + "/";
    else base += version + "/";
    return base + "jasperengine-player-" + id + "-" + version + ".zip";
}

uint64_t PlayerRuntimes::archiveSize(const std::string& id) {
    for (const PlayerArchiveHash& hash : releaseHashes())
        if (id == hash.id) return hash.size;
    return 0;
}

bool PlayerRuntimes::download(const std::string& id, const fs::path& override, const std::atomic<bool>* cancel, std::string& installedId,
                              std::vector<std::string>& warnings, std::string& error, std::atomic<uint64_t>* received) {
    if (!isReleaseEditor() && !std::getenv("JASPER_PLAYERS_URL")) {
        error = "a development editor (" + editorVersion() + ") downloads no players: it trusts only local ones (tools/build.sh players, "
                "--players-dir or JASPER_PLAYERS_DIR, or players install <zip>)";
        return false;
    }
    if (cancel && cancel->load()) {
        error = "cancelled";
        return false;
    }
    htmlui::HttpClient client;
    client.setTimeoutSeconds(1800);
    client.setMaxFileSize(4ll * 1024 * 1024 * 1024);
    // the transfer is watched from a thread of its own: its progress, and a cancel that stops curl
    std::atomic<bool> done{false};
    std::thread watcher([&]() {
        while (!done.load()) {
            if (received) received->store(client.receivedBytes());
            if (cancel && cancel->load()) {
                client.cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const htmlui::HttpResponse response = client.get(downloadUrl(id));
    done.store(true);
    watcher.join();
    if (cancel && cancel->load()) {
        error = "cancelled";
        return false;
    }
    if (!response.ok()) {
        error = "download of " + downloadUrl(id) + " failed: " + response.error;
        return false;
    }
    if (cancel && cancel->load()) {
        error = "cancelled";
        return false;
    }
    if (!install(response.bodyFile, override, installedId, warnings, error)) return false;
    if (installedId != id) {
        error = "the archive of " + id + " holds player " + installedId;
        return false;
    }
    return true;
}
