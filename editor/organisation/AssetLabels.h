// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Asset labels (Unity's), S25 R22-R23: an asset has 0-32 labels, each 1-32 characters from
// [A-Za-z0-9 _.-] after trimming, compared without case and kept with the casing first used.
// They live with the asset's S04 identity: the `labels:` list of its .meta sidecar, or for
// assets with an in-file identity (.scene, .prefab, .material) a top-level `labels:` key right
// after `guid:`/`formerGuids:`, written by a line edit like AssetDatabase::writeGuid (saving an
// open document writes the store's labels through Project::prepareDocumentIdentity, so a save
// keeps them, and a label edit never marks a scene modified). Label colours live in project.yaml
// (`assetLabels: [{name, color}]`); a label without one takes a colour of the 9 defaults
// (OpenEngine's EditorTags.cpp palette) by a stable hash of its lower-case name.
//
// The store reads every asset's labels on a background job when the project opens (search
// with l: sees them as soon as it ends); edits go through setLabels, which the undoable
// commands (organisation/OrganisationCommands.h) call.

#include "asset/Guid.h"

#include "jobs/JobSystem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct LabelColor {
        std::string name;
        std::string color;   // "#RRGGBB"
    };

    class AssetLabels {
    public:
        static constexpr size_t MAX_LABELS = 32;
        static constexpr size_t MAX_LENGTH = 32;

        // A label as stored: trimmed and checked; false with *why
        static bool normalizeLabel(const std::string& raw, std::string& label, std::string* why = nullptr);
        static bool sameLabel(const std::string& a, const std::string& b);
        // Sorted (case-insensitively), duplicates removed, as a YAML flow list "[Hero, Boss Fight]"
        static std::vector<std::string> sortedUnique(std::vector<std::string> labels);
        static std::string formatList(const std::vector<std::string>& labels);
        static std::vector<std::string> parseList(const std::string& yamlValue);

        // The labels of a file on disk: its meta's section (sidecar) or its top-level key (in-file)
        static bool readFileLabels(const std::filesystem::path& absoluteAsset, bool inFile, std::vector<std::string>& labels);
        static std::vector<std::string> readInFileLabels(const std::string& text);
        // An in-file document's text with `labels:` replaced (removed when empty), after its identity
        static std::string withInFileLabels(const std::string& text, const std::vector<std::string>& labels);
        // Writes them (atomically); *written false when the file already held them
        static bool writeFileLabels(const std::filesystem::path& absoluteAsset, bool inFile, const std::vector<std::string>& labels,
                                    std::string* error = nullptr, bool* written = nullptr);

        // The 9 default colours, and the one a label without an entry takes
        static const std::vector<LabelColor>& defaultPalette();
        static std::string paletteColorFor(const std::string& label);
        static bool isColor(const std::string& text);

        ~AssetLabels();

        // The project's assets are read on a job; close() drops everything
        void open(Project* project);
        void close();
        // Main thread, once a frame: takes the job's result when it is done
        void update();
        bool isReady() const;
        void waitUntilReady();
        // Bumped by every change (the search index re-reads labels)
        uint64_t version() const;

        // Thread-safe (scene saves run on a worker)
        std::vector<std::string> labelsOf(const Guid& guid) const;
        // Every asset with labels
        std::vector<std::pair<Guid, std::vector<std::string>>> all() const;

        struct LabelInfo {
            std::string name;
            std::string color;
            size_t assets = 0;
            bool hasEntry = false;   // project.yaml lists it
        };
        // The labels in use and the ones project.yaml lists, by name
        std::vector<LabelInfo> labels() const;
        std::string colorOf(const std::string& label) const;
        // The spelling a label is known by (an entry, then an asset's), else `label` as given
        std::string canonical(const std::string& label) const;

        struct WriteResult {
            bool ok = false;
            std::string error;
            std::vector<std::string> filesWritten;   // project-relative
        };
        // New label lists for assets, written to their files: all or nothing (a failed write
        // puts the files written before it back, D5). Labels are normalised and sorted.
        WriteResult setLabels(const std::vector<std::pair<Guid, std::vector<std::string>>>& changes);
        // A changed asset file (the watcher, an editor save): its labels are read again
        void refreshAsset(const Guid& guid);

        // project.yaml's entries (Stream encodes and decodes them)
        const std::vector<LabelColor>& entries() const { return palette; }
        void setEntries(std::vector<LabelColor> entries);

    private:
        Project* project = nullptr;
        mutable std::mutex mutex;
        std::unordered_map<Guid, std::vector<std::string>> byGuid;
        std::unordered_set<Guid> touchedWhileLoading;
        std::vector<LabelColor> palette;
        uint64_t revision = 1;
        bool ready = false;

        struct Pending {
            std::unordered_map<Guid, std::vector<std::string>> labels;
        };
        jobs::Task<std::shared_ptr<Pending>> loading;
        bool loadingStarted = false;

        bool assetFile(const Guid& guid, std::filesystem::path& absolute, bool& inFile, std::string& relative) const;
    };

}
