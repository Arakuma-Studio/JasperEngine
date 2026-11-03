// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Releases of a project's builds (docs/specs/S30-shipping.md R22-R26, D10, D11): publishing.json
// (the targets, Steam and the Steam Deck), `prepare` (validate, copy, split the debug symbols,
// re-validate, release.json, rename into place), `verify` (the payload checks again and the tree
// against release.json), the SteamPipe scripts and their upload with SteamCMD, and a Deck deploy
// over the system's ssh/scp with one previous build kept. One implementation backs the UI, the
// CLI and MCP. Portions adapted from OpenEngine Tools/Scripts/publish_game.py (MIT).

#include "publish/TreeManifest.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::publish {

    struct PublishTarget {
        std::string platform = "linux";      // windows linux macos android ios web
        std::string arch = "x64";
        std::string backend;
        std::string flavour = "release";     // release development
        std::string buildDir;                // project-relative (or absolute): the build's output
        std::string executable;              // payload-relative
        std::string depotId;                 // Steam depot
        std::string deviceProfile;           // "steamdeck"
        std::vector<std::string> requiredFiles;
    };

    struct SteamSettings {
        std::string appId;
        std::string buildDescription;
    };

    struct DeckSettings {
        std::string host;
        std::string user = "deck";
        std::string remoteDir = "Games/jasperengine";   // relative to the user's home
        std::string keyFile;
        int port = 0;                                   // 0: ssh's default
    };

    struct PublishingConfig {
        static constexpr int SCHEMA_VERSION = 1;

        std::string releasesDir = "releases";           // project-relative (or absolute)
        std::map<std::string, PublishTarget> targets;
        SteamSettings steam;
        DeckSettings deck;

        static std::filesystem::path pathOf(const std::filesystem::path& projectDir) { return projectDir / "publishing.json"; }
        static bool load(const std::filesystem::path& file, PublishingConfig& out, std::string& error);
        static bool parse(const std::string& text, PublishingConfig& out, std::string& error);
        std::string toJson() const;
        bool save(const std::filesystem::path& file, std::string& error) const;
        // problems of the settings themselves (names, ids, paths), each a sentence
        std::vector<std::string> check() const;
    };

    // a starting publishing.json: one target per platform of `builtDirs` (name -> build dir)
    PublishingConfig defaultPublishingConfig(const std::map<std::string, std::string>& builtDirs);

    struct ReleaseTarget {
        std::string platform, arch, backend, flavour, playerId, method, executable, depotId, deviceProfile, glibcFloor;
        std::map<std::string, TreeEntry> files;
    };

    struct ReleaseInfo {
        static constexpr int SCHEMA_VERSION = 1;

        std::string release;
        std::string project;
        std::string engineVersion;
        std::string createdUtc;
        std::string steamAppId;
        std::map<std::string, ReleaseTarget> targets;

        static bool load(const std::filesystem::path& releaseDir, ReleaseInfo& out, std::string& error);
        std::string toJson() const;
    };

    // a release, target or branch name: letters, digits, '.', '_', '-', starting with a letter or digit
    bool isIdentifier(const std::string& value);

    // the tools a release step runs; null members: the system's (ExternalTools)
    struct PublishTools {
        CommandRunner* runner = nullptr;
        std::function<void(const std::string&)> log;
        std::function<bool()> cancelled;
    };

    struct PrepareRequest {
        std::filesystem::path projectDir;
        std::string projectName;
        PublishingConfig config;
        std::string release;
        std::vector<std::string> targets;    // empty: all
    };

    struct PrepareResult {
        std::filesystem::path releaseDir;
        std::vector<std::string> warnings;   // W-SYMBOLS-NOT-SPLIT ...
    };

    bool prepareRelease(const PrepareRequest& request, const PublishTools& tools, PrepareResult& result, std::vector<std::string>& problems);
    // the release's targets pass R30 again and hold exactly what release.json lists
    bool verifyRelease(const std::filesystem::path& releaseDir, std::vector<std::string>& problems, ReleaseInfo* info = nullptr);

    // SteamPipe: steampipe/app_build_<appId>.vdf and depot_build_<depot>.vdf of a verified release
    std::string vdfString(const std::string& value, std::string& error);
    bool writeSteamScripts(const std::filesystem::path& releaseDir, bool preview, const std::string& branch,
                           std::filesystem::path& appScript, std::vector<std::filesystem::path>& depotScripts, std::string& error);
    // verify, the scripts (not a preview), then `steamcmd +login <user> +run_app_build <vdf> +quit`;
    // the login is SteamCMD's cached session (the editor never sees credentials)
    bool steamUpload(const std::filesystem::path& releaseDir, const std::string& user, const std::string& branch,
                     const std::filesystem::path& steamcmd, const PublishTools& tools, std::string& error);

    // The Deck (any ssh host): the release's linux-x64 target copied to
    // <remoteDir>/<release>.incoming, its checksums checked there, swapped in as current/ with
    // the build before kept as previous/
    bool deckDeploy(const std::filesystem::path& releaseDir, const std::string& target, const DeckSettings& deck,
                    const PublishTools& tools, std::string& output, std::string& error);
    bool deckRollback(const DeckSettings& deck, const PublishTools& tools, std::string& output, std::string& error);
    // runs current/<executable> --quit-after 600 --fail-on-error there: its exit code and log tail
    bool deckSmoke(const DeckSettings& deck, const std::string& executable, const PublishTools& tools, int& exitCode,
                   std::string& output, std::string& error);

    // the newest GLIBC_x.y the ELF files of a tree need
    std::string treeGlibcFloor(const std::filesystem::path& root);

}
