// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The prebuilt player runtimes a build uses (docs/specs/S30-shipping.md R8-R10, D4, D5): one per
// <platform>-<arch>-<backend>-<flavour>, and per release variant (-<suffix>, a module set of
// engine/player/variants.json: docs/specs/S34-player-size.md R4), each a directory with its binaries, licenses/, its
// platform template and player-info.json. Found in --players-dir / JASPER_PLAYERS_DIR, next to
// the editor (its install ships the host's desktop players and the web's), and in
// <user cache>/jasperengine/players/<editor version>/ where downloads and installs go.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct PlayerInfo {
        static constexpr int SCHEMA_VERSION = 1;

        std::string id;
        std::string platform;          // windows linux macos android ios web
        std::string arch;
        std::string backend;
        std::string flavour;           // release development
        std::string engineVersion;
        std::string gitSha;
        std::string contentFormatVersion;
        // the cooked model formats it decodes (CookedModels::kFormatVersion, S53 R30); empty in a
        // runtime built before model cooks
        std::vector<int> modelCookFormats;
        std::map<std::string, unsigned int> capacities;   // MAX_SUBMESHES, ...
        std::vector<std::string> features;                // the modules (htmlui video runtimeshaders physics3d ...), tracy
        std::string variant = "full";                     // the release variant it was built for
        std::vector<std::string> notices;                 // licenses/<lib>-LICENSE of each
        // binaries (runtime-relative) and their sha256, re-checked by install
        std::map<std::string, std::string> executables;
        // the main binary or bundle the build stages (jasperengine-player, JasperEnginePlayer.app...)
        std::string executable;

        std::filesystem::path dir;     // where it is installed
        uint64_t sizeBytes = 0;
        uint64_t shippedBytes = 0;     // its binaries (what a build stages of it)

        bool hasModules(const std::vector<std::string>& modules) const;

        static bool load(const std::filesystem::path& dir, PlayerInfo& out, std::string& error);
        static bool fromJson(const std::string& text, PlayerInfo& out, std::string& error);
        std::string toJson() const;
    };

    // one release's archive hashes (PlayerRuntimeHashes.gen.h): empty in a development editor
    struct PlayerArchiveHash {
        const char* id;
        const char* sha256;
        uint64_t size;
    };

    // a release variant of engine/player/variants.json
    struct PlayerVariant {
        std::string name;
        std::string suffix;                // of the id; empty for the full player
        std::vector<std::string> modules;
    };

    class PlayerRuntimes {
    public:
        // "<platform>-<arch>-<backend>-<flavour>[-<suffix>]"
        static std::string runtimeId(const std::string& platform, const std::string& arch, const std::string& backend, bool development,
                                     const std::string& variantSuffix = "");
        // the release variants (the editor's engine/player/variants.json; the full player alone
        // when it cannot be read), the full one first
        static const std::vector<PlayerVariant>& variants();
        // The release variant a project needing these modules ships with: the one of fewest
        // modules holding all of them (the full player always does), of those the last listed
        static const PlayerVariant& variantFor(const std::vector<std::string>& modules);
        // the editor's version, and whether it is a release (an exact v<major>.<minor>.<patch>)
        static std::string editorVersion();
        static bool isReleaseEditor();

        // where runtimes are looked for, in order; `override` (--players-dir) first
        static std::vector<std::filesystem::path> searchDirs(const std::filesystem::path& override = {});
        // where installs and downloads go
        static std::filesystem::path installRoot(const std::filesystem::path& override = {});

        static std::vector<PlayerInfo> list(const std::filesystem::path& override = {});
        // the runtime of that id this editor may use (R10: its engineVersion, a content format of
        // the same major), else nullopt with why; allowMismatch takes any engine version
        static std::optional<PlayerInfo> find(const std::string& id, const std::filesystem::path& override, bool allowMismatch,
                                              std::string& error);
        // every installed runtime this editor may use of a <platform>-<arch>-<backend>-<flavour>
        // (`baseId`): the full player and each release variant
        static std::vector<PlayerInfo> findVariants(const std::string& baseId, const std::filesystem::path& override, bool allowMismatch);
        // of `candidates`, the one that stages the fewest bytes and holds every module (nullptr: none)
        static const PlayerInfo* smallestWith(const std::vector<PlayerInfo>& candidates, const std::vector<std::string>& modules);

        // A runtime archive (jasperengine-player-<id>-<version>.zip): checked against this
        // release's hash (a development editor has none: the archive's own player-info hashes
        // only, with a warning), extracted to a temporary sibling, its binaries re-hashed, then
        // renamed into place atomically. Refuses entries outside the archive.
        static bool install(const std::filesystem::path& archive, const std::filesystem::path& override, std::string& installedId,
                            std::vector<std::string>& warnings, std::string& error,
                            const std::vector<PlayerArchiveHash>* hashes = nullptr);
        static bool remove(const std::string& id, const std::filesystem::path& override, std::string& error);

        // the release's archive of that id from the editor's GitHub release (R9); a development
        // editor cannot download (it trusts only local players)
        static std::string downloadUrl(const std::string& id);
        // cancel stops the transfer itself; received (when given) follows the bytes that arrived
        static bool download(const std::string& id, const std::filesystem::path& override, const std::atomic<bool>* cancel,
                             std::string& installedId, std::vector<std::string>& warnings, std::string& error,
                             std::atomic<uint64_t>* received = nullptr);
        // the release archive's size of that id (0 when this editor has no table entry)
        static uint64_t archiveSize(const std::string& id);

        // this release's archive hashes
        static const std::vector<PlayerArchiveHash>& releaseHashes();
    };

}
