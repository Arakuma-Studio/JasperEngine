// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The in-memory asset index the Project window's search, saved searches, the palette's asset:
// scope and MCP's list_assets search evaluate AssetQuery over (S25 R21): every asset of S04's
// AssetDatabase with its labels, plus the project's folders. It follows the database's version
// (its change events and S05's watcher reach it there) and the label store's; the folders come
// from the assets' paths and a directory walk that runs on a job when the database changed.
// A search itself never touches the file system.

#include "AssetQuery.h"

#include "jobs/JobSystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // A frozen list of records (the palette's snapshot, tests): missing: answers from the set
    // given (computed on the main thread when the snapshot was made); ref: needs the live index
    class AssetRecordsView final : public AssetIndexView {
    public:
        explicit AssetRecordsView(std::shared_ptr<const std::vector<AssetRecord>> records,
                                  std::unordered_set<std::string> missing = {});
        size_t size() const override { return records->size(); }
        const AssetRecord& at(size_t index) const override { return (*records)[index]; }
        std::unordered_set<std::string> referrersOf(const std::string& pathOrGuid) const override;
        std::unordered_set<std::string> withMissingReferences() const override { return missing; }
    private:
        std::shared_ptr<const std::vector<AssetRecord>> records;
        std::unordered_set<std::string> missing;
    };

    class AssetSearchIndex final : public AssetIndexView {
    public:
        ~AssetSearchIndex() override;

        void attach(Project* project);
        void detach();
        // Main thread: the records brought up to date (cheap when nothing changed)
        void refresh();
        uint64_t version() const { return revision; }

        size_t size() const override { return records->size(); }
        const AssetRecord& at(size_t index) const override { return (*records)[index]; }
        // The records as they are now, shared: a rebuild makes a new list, so a snapshot (the
        // Search palette's job) keeps reading this one
        std::shared_ptr<const std::vector<AssetRecord>> shared() const { return records; }
        std::unordered_set<std::string> referrersOf(const std::string& pathOrGuid) const override;
        std::unordered_set<std::string> withMissingReferences() const override;

        const AssetRecord* findByPath(const std::string& projectRelative) const;
        const AssetRecord* findByGuid(const std::string& guid) const;

    private:
        Project* project = nullptr;
        std::shared_ptr<const std::vector<AssetRecord>> records = std::make_shared<std::vector<AssetRecord>>();
        std::unordered_map<std::string, size_t> byPath;
        std::unordered_map<std::string, size_t> byGuid;
        uint64_t databaseVersion = 0;
        uint64_t labelsVersion = 0;
        uint64_t revision = 1;
        std::vector<std::string> walkedFolders;
        jobs::Task<std::shared_ptr<std::vector<std::string>>> folderWalk;
        bool walking = false;
        bool walkWanted = true;

        void startFolderWalk();
        void rebuild();
    };

}
