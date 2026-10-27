// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An in-memory AssetIndexView for the search tests: no project, no file system. ref: and
// missing: answer from the maps the test fills, and count their calls.

#include "search/AssetQuery.h"

#include <cctype>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor::test {

    class FakeAssetIndex final : public AssetIndexView {
    public:
        std::vector<AssetRecord> records;
        std::map<std::string, std::unordered_set<std::string>> referrers;   // path or GUID -> referrers
        std::unordered_set<std::string> missing;
        mutable int referrerCalls = 0;
        mutable int missingCalls = 0;

        size_t size() const override { return records.size(); }
        const AssetRecord& at(size_t index) const override { return records[index]; }
        std::unordered_set<std::string> referrersOf(const std::string& pathOrGuid) const override {
            referrerCalls++;
            auto it = referrers.find(pathOrGuid);
            return it == referrers.end() ? std::unordered_set<std::string>{} : it->second;
        }
        std::unordered_set<std::string> withMissingReferences() const override {
            missingCalls++;
            return missing;
        }

        // A file ("textures/hero.png", GUID "guid-<path>") or, with type "folder", a folder
        AssetRecord& add(const std::string& path, const std::string& type, std::vector<std::string> labels = {}) {
            AssetRecord record;
            record.path = path;
            record.folder = type == "folder";
            record.guid = record.folder ? "" : "guid-" + path;
            record.name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
            const size_t dot = record.name.rfind('.');
            record.ext = record.folder || dot == std::string::npos ? "" : record.name.substr(dot);
            for (char& c : record.ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            record.type = type;
            record.labels = std::move(labels);
            prepareAssetRecord(record);
            records.push_back(record);
            return records.back();
        }

        // The paths of evaluate's matches, in order
        std::vector<std::string> paths(const std::vector<AssetQuery::Match>& matches) const {
            std::vector<std::string> out;
            for (const AssetQuery::Match& match : matches) out.push_back(records[match.index].path);
            return out;
        }
    };

}
