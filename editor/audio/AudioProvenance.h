// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where a clip came from (spec S36 R8): the top-level `provenance` section of its S04 meta sidecar
// (opaque to S04). A licence is not an import setting, and other asset types can use it later.
//
//   provenance:
//     version: 1
//     licence: CC0-1.0                 # SPDX id, or generated | unknown | proprietary
//     licence_url: http://creativecommons.org/publicdomain/zero/1.0/
//     author: Kenney (www.kenney.nl)
//     attribution: Kenney (www.kenney.nl)
//     source: kenney                   # kenney | freesound | generate_sound | user | <free text>
//     url: https://kenney.nl/assets/impact-sounds
//     builtin: {set: essentials, id: footstep/concrete_1, sha256: d7267e18...}
//     generator: {preset: jump, seed: 1234, params: {}, loudness_lufs: -18, version: 1}
//     reference: {path: ..., licence: ..., author: ..., url: ...}

#pragma once

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class AudioProvenance {
    public:
        using Json = nlohmann::json;

        static constexpr const char* metaKey = "provenance";
        static constexpr int currentVersion = 1;

        static constexpr const char* LICENCE_GENERATED = "generated";
        static constexpr const char* LICENCE_UNKNOWN = "unknown";
        static constexpr const char* LICENCE_PROPRIETARY = "proprietary";

        // Every key as it reads in the section, in R8 order, then the unknown keys in file order.
        // `builtin`, `generator` and `reference` are objects.
        Json values = Json::object();

        // no key but `version` would be written
        bool isEmpty() const { return values.empty(); }
        std::string text(const char* key) const;
        std::string licence() const { return text("licence"); }

        // Reads a section; a mapping is all it asks of it. An absent or null node is empty.
        static bool parse(const YAML::Node& section, AudioProvenance& out, std::string& error);
        // The section of an asset's .meta. True and empty when there is none.
        static bool load(const std::filesystem::path& asset, AudioProvenance& out, std::string& error);
        // The section's value, keys in order; a null node when empty, which removes the section
        YAML::Node toYaml() const;
        // Keys set, null removing one; a known key of the wrong shape fails with the reason
        bool apply(const Json& keys, std::string& error);
        // The R8 key names, in order
        static const std::vector<std::string>& keyNames();
        // The licence as an analysis item shows it: {id, source[, author, url, attribution, licence_url]}
        Json licenceJson() const;
    };

    // The text of a clip's .meta with these top-level sections (a YAML map {import, provenance,
    // labels}): an existing .meta keeps its GUID and its other sections, a new one gets a minted
    // GUID. False, with the reason, when the existing file does not read as a meta.
    bool clipMetaText(const std::filesystem::path& asset, const YAML::Node& sections, std::string& text, std::string& error);

}
