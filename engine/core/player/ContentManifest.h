// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// content-manifest.json v1 (docs/specs/S30-shipping.md D4, R21): what a build shipped, beside
// game.json. The editor writes it last (Prebuilt and Compile builds alike); a player checks it
// before it starts (exit 5 on a mismatch), and `publish verify` compares a release with it.

#include "Export.h"

#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::player {

    struct ManifestFile {
        std::string sha256;
        uint64_t size = 0;
    };

    struct ManifestPack {
        std::string file;
        std::string sha256;
        uint64_t entries = 0;
    };

    struct JASPERENGINE_API ContentManifest {
        static constexpr int SCHEMA_VERSION = 1;

        std::string engineVersion;
        std::string contentFormatVersion;
        std::string playerId;              // empty for a Compile build
        std::string playerSha256;
        std::string method;                // "prebuilt" or "compile"
        std::string backend;               // glcore, gles3, vulkan, d3d11, metal
        uint32_t startScene = 0;
        std::vector<std::string> scenes;   // content-relative cooked files
        std::vector<std::string> prefabs;
        std::vector<std::string> shaders;  // .sdat names under assets/shaders
        std::map<std::string, ManifestFile> files;   // loose files, content-relative
        std::vector<ManifestPack> packs;
        // the libraries whose licenses/<name>-LICENSE the build carries (R27)
        std::vector<std::string> notices;
        // the optional engine modules the content needs (docs/specs/S34-player-size.md R3);
        // absent in a manifest of an older editor, whose content needs every module
        std::vector<std::string> modules;
        bool hasModules = false;
        bool allowPlayerMismatch = false;
        // a Compile release build's split debug information (absolute; the build's machine): what
        // publish prepare copies into the release's symbols/; empty for a prebuilt build, whose
        // player runtime holds its own
        std::string symbols;

        nlohmann::json toJson() const;
        static bool fromJson(const nlohmann::json& json, ContentManifest& out, std::string& error);
        static bool parse(const std::string& text, ContentManifest& out, std::string& error);
    };

    // R21: the checks a player makes at start. `playerId`: this player's runtime id (empty: a
    // Compile build, which has none to compare); `exists` tells whether a content-relative file
    // is there (on disk or in the mounted pack); `verifyHashes` (--verify-content) re-hashes every
    // loose file, read through `read`. `playerModules`: the modules of the player (null: this
    // build's, player::builtModules), which must hold every module the content needs (S34 R3).
    // Every problem goes to `problems`; true when there are none.
    struct ContentAccess {
        bool (*exists)(const std::string& relative);
        bool (*sha256)(const std::string& relative, std::string& hex, uint64_t& size);
    };
    JASPERENGINE_API bool checkContent(const ContentManifest& manifest, const std::string& playerId, const ContentAccess& access,
                                       bool verifyHashes, std::vector<std::string>& problems,
                                       const std::vector<std::string>* playerModules = nullptr);
    // the modules the content needs that `available` lacks (every module when the manifest names none)
    JASPERENGINE_API std::vector<std::string> missingModules(const ContentManifest& manifest, const std::vector<std::string>& available);

    // this player's runtime id, "<platform>-<arch>-<backend>-<flavour>[-<variant>]" (empty in a Compile build)
    JASPERENGINE_API std::string runtimePlayerId();

}
