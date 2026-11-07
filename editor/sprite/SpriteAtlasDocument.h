// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .spriteatlas file (docs/specs/S45-sprite-atlas.md R13, R38, §6.5): canonical JSON with its
// GUID in the file. Members are files (a path and the GUID that finds it again after a rename)
// and folders (paths, recursive, filtered by type), with an exclude list of path prefixes.
// - parse: `format` and `version` checked (a newer version is refused read-only, `atlas_version`);
//   bad values are problems and fall back to their defaults; unknown keys are kept and written
//   back where they were (top level, settings, a member).
// - write: keys in §6.5's order, unknown keys after them, one member per line; write(parse(t))
//   == t for every text write made. Pure (any thread).

#pragma once

#include "pixelart/PixelProblem.h"

#include "asset/Guid.h"

#include "json.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::sprite {

    constexpr const char* kSpriteAtlasFormat = "jasperengine.spriteatlas";
    constexpr int kSpriteAtlasVersion = 1;

    // the platform families an atlas may size differently (R14, R19), in the inspector's order
    constexpr const char* kAtlasPlatforms[] = {"desktop", "mobile", "web", "android_etc2"};

    struct AtlasPlatformOverride {
        bool override = false;
        int maxSize = 2048;
    };

    struct SpriteAtlasSettings {
        int maxSize = 2048;           // 32..8192
        int padding = 1;              // 0..16
        int extrude = 1;              // 0..4
        bool trim = true;
        bool keepMembersTogether = true;
        bool powerOfTwo = false;
        bool square = false;
        std::string compression = "auto";   // auto | lossless | platform_default
        std::string filter = "auto";        // auto | point | bilinear
        std::map<std::string, AtlasPlatformOverride> platforms;
        nlohmann::ordered_json unknown = nlohmann::ordered_json::object();

        // the max size of a platform family ("" or "default": the default)
        int maxSizeFor(const std::string& platform) const;
    };

    struct SpriteAtlasMember {
        std::string path;             // a file member (asset-relative), or
        Guid guid;
        std::string folder;           // a folder member (asset-relative)
        std::vector<std::string> types;   // a folder's types ("texture", "aseprite_sprite"); empty: both
        nlohmann::ordered_json unknown = nlohmann::ordered_json::object();

        bool isFolder() const { return !folder.empty(); }
    };

    struct SpriteAtlasDocument {
        Guid guid;
        bool includeInBuild = true;
        std::vector<SpriteAtlasMember> members;
        std::vector<std::string> exclude;
        SpriteAtlasSettings settings;
        nlohmann::ordered_json unknown = nlohmann::ordered_json::object();
    };

    struct SpriteAtlasParseResult {
        std::optional<SpriteAtlasDocument> document;
        pixelart::PixelProblem error;           // not JSON, not an atlas, `atlas_version`
        std::vector<pixelart::PixelProblem> problems;   // values dropped (their defaults used)
        bool ok() const { return document.has_value(); }
    };

    SpriteAtlasParseResult parseSpriteAtlas(const std::string& text);
    std::string writeSpriteAtlas(const SpriteAtlasDocument& document);

    // A partial settings edit (MCP `settings`, the inspector): the keys of §6.5's settings, for
    // one platform's override when `platform` is set (max_size only). False with problems
    // (`invalid_settings`, field the key) and `out` unchanged.
    bool applyAtlasSettings(const nlohmann::json& edit, const std::string& platform, SpriteAtlasSettings& out,
                            std::vector<pixelart::PixelProblem>& problems);
    nlohmann::json atlasSettingsJson(const SpriteAtlasSettings& settings);
    bool isAtlasPlatform(const std::string& platform);

}
