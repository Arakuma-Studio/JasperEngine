// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `<file name>.meta` sidecar of an asset without an in-file identity:
//
//   metaVersion: 1
//   guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13
//   formerGuids: [9c1d0f3e5b7a42c8a1e06d2f4b8c7a19]   # optional, sorted
//   subAssets:                                         # models only, sorted by key
//     - {key: "mesh:Body", guid: 7f..., name: Body}
//   import: {...}                                      # S14, opaque here
//   labels: [...]                                      # S25, opaque here
//
// Sections this code does not own (import, labels, anything a newer editor wrote) are
// kept byte for byte, in their place; a known section is re-emitted only when its value
// changed. Metas hold no volatile data (no mtimes, hashes or machine paths), are written
// with LF line endings, UTF-8 without BOM, and only when their content changes.

#pragma once

#include "asset/Guid.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class AssetMeta {
    public:
        struct SubAsset {
            std::string key;
            Guid guid;
            std::string name;

            bool operator==(const SubAsset& other) const { return key == other.key && guid == other.guid && name == other.name; }
        };

        static constexpr int currentVersion = 1;

        int metaVersion = currentVersion;
        Guid guid;
        std::vector<Guid> formerGuids;      // kept sorted
        std::vector<SubAsset> subAssets;    // kept sorted by key

        // False when the text is not a meta (no valid guid), with a reason
        static bool parse(const std::string& text, AssetMeta& meta, std::string* error = nullptr);
        std::string serialize() const;

        // Opaque top-level sections ("import", "labels", ...): the raw YAML of the value as
        // written after "<name>:" (a flow value on the key line, or the indented block below)
        bool hasSection(const std::string& name) const;
        std::string getSection(const std::string& name) const;
        void setSection(const std::string& name, const std::string& valueYaml);
        void removeSection(const std::string& name);

        // metaVersion, guid, formerGuids, subAssets: the sections this class writes itself
        static bool isKnownSection(const std::string& name) { return isKnown(name); }

        void addFormerGuid(const Guid& former);
        bool removeFormerGuid(const Guid& former);
        void setSubAssets(std::vector<SubAsset> entries);

    private:
        struct Section {
            std::string name;
            std::string text;      // the section's lines, key line first, '\n' ended
        };
        std::vector<Section> sections;   // in file order, the known ones included

        // The parsed values of the known sections, to tell which ones changed
        int parsedVersion = -1;
        Guid parsedGuid;
        std::vector<Guid> parsedFormer;
        std::vector<SubAsset> parsedSubAssets;

        static std::string canonical(const std::string& name, const AssetMeta& meta);
        static bool isKnown(const std::string& name);
    };

}
