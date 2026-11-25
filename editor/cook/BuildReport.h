// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What an export ships and how big it is (spec S53 R21): totals by category (models, textures,
// audio, scripts, scenes, engine, other) and the largest files with their cooked and source
// sizes. Measured on the files themselves before they are packed; written as build-report.json
// next to the export, shown on the export's completion page and in Window > Analysis > Build
// Report. docs/specs/build-size-tools.md adds every file, what the export left out, why each
// asset ships (the dependency index walked from project.yaml and the scripts), the size budgets'
// results and the comparison of two reports; the file stays version 1 with optional keys.

#pragma once

#include "mcp/McpJson.h"
#include "model/ModelImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    using mcp::Json;

    struct BuildReportFile {
        std::string path;           // in the build ("assets/je-cooked/models/3f...glb")
        std::string category;
        uint64_t bytes = 0;
        std::string source;         // the asset it was cooked from (empty: shipped as it is)
        uint64_t sourceBytes = 0;
        // the project asset it ships, relative to the assets root ("" none: see origin)
        std::string asset;
        std::string origin;         // "" an asset's, "engine", "generated" (the asset map, cook indexes...)
    };

    // A source the export did not ship as itself (R2)
    struct BuildReportLeftOut {
        std::string source;         // relative to the assets root
        uint64_t bytes = 0;
        std::string reason;         // cooked_texture, atlas_only, profile_excluded... (leftOutReasonText)
    };

    // How the walk from the roots first reached an asset (R3): the referrer, the field locator
    // and the edge kind; a root has kind "root" and no referrer
    struct BuildReportReach {
        std::string via, field, kind;
        std::string object;         // the GameObject holding the field ("Root/Lamp"), "" outside GameObjects
        bool operator==(const BuildReportReach& other) const {
            return via == other.via && field == other.field && kind == other.kind && object == other.object;
        }
    };

    // One size budget row as the build met it (R13)
    struct BuildReportBudget {
        std::string platform, category;
        uint64_t limitBytes = 0, bytes = 0;
        uint64_t overBytes() const { return bytes > limitBytes ? bytes - limitBytes : 0; }
    };

    // A step of a Referenced By chain: the asset, and the field and edge kind that reach it
    struct BuildReportStep {
        std::string asset, field, kind, object;
    };

    // Why a file or asset ships (R9): reason "reached" (with the chain to a root), "unreferenced",
    // "engine", "generated", or "not_in_build"
    struct BuildReportReferrers {
        std::string file, asset, reason;
        std::vector<BuildReportStep> chain;
    };

    // An asset Asset Stripping left out (S98 REQ-10): project-relative, its bytes on disk, and why
    // ("unreferenced" under Asset Stripping: Referenced Only, or "excluded: <glob>")
    struct BuildReportStripped {
        std::string path;
        uint64_t bytes = 0;
        std::string reason;
    };

    class BuildReport {
    public:
        static constexpr const char* kFileName = "build-report.json";
        static constexpr size_t kLargest = 25;

        std::string platform;           // "linux", "web"...
        std::string method;             // "prebuilt", "compile", "source"
        std::string buildProfile;       // the Build Profile ("" none: absent from the file)
        uint64_t totalBytes = 0;
        std::map<std::string, uint64_t> categories;
        std::vector<BuildReportFile> largest;
        // build-size-tools: the report's identity (the history id), when (UTC, ISO 8601) and which editor
        std::string id, created, editorVersion;
        std::vector<BuildReportFile> files;         // every file (written in path order)
        bool hasFiles = false;                      // false: a report from before the list (compare is partial)
        std::vector<BuildReportLeftOut> leftOut;    // in source path order
        // asset -> how it was reached; absent: the index was not ready or the report predates it
        std::optional<std::map<std::string, BuildReportReach>> reachability;
        std::vector<BuildReportBudget> budgets;
        // S98 REQ-10: the assets Asset Stripping and Exclude From Build left out, in path order
        std::vector<BuildReportStripped> stripped;
        // a web build's download estimate (S99 R10: webDownloadGzipBytes); 0 for other builds
        uint64_t downloadBytesGzip = 0;

        // the stripped ("unreferenced") and excluded totals
        size_t strippedCount() const { return stripped.size(); }
        uint64_t strippedBytes() const;

        // the categories in display order
        static const std::vector<std::string>& categoryNames();
        // "assets/je-cooked/models/a.glb" -> "models", "lua/main.lua" -> "scripts"...
        static std::string categoryOf(const std::string& path);

        // Adds every file below `root` (paths reported relative to it). `cooked` maps a build
        // path to its source asset and that source's bytes (the model and texture cooks'
        // indexes); files whose category is in `only` when it is not empty.
        void addTree(const std::filesystem::path& root, const std::map<std::string, std::pair<std::string, uint64_t>>& cooked,
                     const std::vector<std::string>& only = {}, const std::string& prefix = {});
        void addFile(const BuildReportFile& file);
        uint64_t leftOutBytes() const;
        // "the cooks replaced it" for cooked_texture...
        static std::string leftOutReasonText(const std::string& reason);

        // R9: a file (its build path) or an asset; the chain follows reachability (at most 32
        // steps, a repeat ends it), the last step a root
        BuildReportReferrers referencedBy(const std::string& fileOrAsset) const;
        std::vector<BuildReportStep> chainOf(const std::string& asset) const;
        // R11: the shipped files no root reaches, largest first (none without reachability)
        std::vector<const BuildReportFile*> unreferenced() const;
        static const char* unreferencedNote();

        // the cooked textures of a build's assets folder: build path -> (source, its bytes on disk)
        static std::map<std::string, std::pair<std::string, uint64_t>> cookedTextures(const std::filesystem::path& assetsDir,
                                                                                       const std::filesystem::path& sourceAssets);

        // every category in display order, 0 for one with no files (as build-report.json has them)
        static Json categoriesJson(const std::map<std::string, uint64_t>& categories);
        // build-report.json's text: keys in order, one line per file, left-out source and reached asset
        std::string toText() const;
        // false for text that is not a version 1 report (unknown keys are skipped); comparisonOnly
        // reads what compareBuildReports needs (no reachability, left-out sources or budgets)
        static bool parse(const std::string& text, BuildReport& out, bool comparisonOnly = false);
        bool write(const std::filesystem::path& file, std::string& error) const;
        static bool load(const std::filesystem::path& file, BuildReport& out, std::string& error, bool comparisonOnly = false);
        // "Build Report (Web): 142 MB; textures 61 MB, models 38 MB..."
        std::string summary() const;
        static std::string formatBytes(uint64_t bytes);

        // S99 R10: whether a browser fetches this file of a web build (a path relative to the
        // build): not the notices, the manifests, this report, or the multithreaded player, which
        // only a cross-origin isolated page loads instead of the single-threaded one
        static bool isWebDownload(const std::string& path);
        // gzip -9 of each file a browser fetches below `root`, as a server compresses it
        // (tools/players/player_sizes.py measures the web players so): an estimate, since the
        // server picks the level and the encoding
        static uint64_t webDownloadGzipBytes(const std::filesystem::path& root);

        // S99 R11: the Download Budget family of a report's platform ("web", "android", "linux",
        // "desktop"...); false for a source export, which downloads nothing
        static bool budgetFamilyOf(const std::string& platform, ModelFamily& family);
        // what the Download Budget holds a build to: a web build's gzip estimate, else its bytes
        uint64_t budgetedBytes() const;
        // a budget in MB (MiB, as formatBytes prints them) in bytes; 0 for 0 or less
        static uint64_t budgetBytesOf(double megabytes);
        // the non-empty categories, largest first, at most `count`
        std::vector<std::pair<std::string, uint64_t>> largestCategories(size_t count) const;
        // W-BUILD-OVER-BUDGET's message: the budget, the actual size and the three largest
        // categories; "" when budgetBytes is 0 or the build fits
        std::string overBudgetMessage(uint64_t budgetBytes) const;

        // the report the last export of this editor wrote (the Build Report window opens it)
        static void setLast(const std::filesystem::path& file);
        static std::filesystem::path last();
    };

    // R7: one row of a comparison, keyed by the source asset when the file has one, else its path
    struct BuildReportChange {
        std::string key, path, category, change;   // change: added, removed, grown, shrunk
        uint64_t before = 0, after = 0;
        int64_t delta = 0;
    };

    struct BuildReportDiff {
        std::map<std::string, std::pair<uint64_t, uint64_t>> categories;   // before, after
        uint64_t totalBefore = 0, totalAfter = 0;
        std::vector<BuildReportChange> changes;    // |delta| descending, then key
        size_t unchanged = 0;
        size_t added = 0, removed = 0, grown = 0, shrunk = 0;
        bool partial = false;                      // the base has no file list: its 25 largest only
        bool platformDiffers = false;
    };

    BuildReportDiff compareBuildReports(const BuildReport& base, const BuildReport& head);

}
