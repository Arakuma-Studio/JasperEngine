// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's side of .aseprite imports (docs/specs/S44-aseprite-import.md R11, R21-R23, R27,
// §6.5): the import of each AsepriteSprite asset runs on a Background job, its outputs go to a
// content-addressed cache, and the editor's ImportedAssets provider answers "<file>#sheet",
// "<file>#image#<n>" and "<file>#animation:<tag>" from it.
//
//   .jasperengine/cache/imported/v1/<k[0:2]>/<k>/sheet.json      the jasperengine.spritesheet record
//   .jasperengine/cache/imported/v1/<k[0:2]>/<k>/page<n>.png     its pages (RGBA8, lossless)
//   .jasperengine/cache/imported/v1/<k[0:2]>/<k>/clip-<n>.anim   the tag clips (canonical .anim)
//   .jasperengine/cache/imported/v1/<k[0:2]>/<k>/import.json     warnings, timings, sub-assets (written last)
//
// k = SHA-256 of (the file's content hash, its asset path and GUID, the resolved settings, the
// pixels per unit, the importer and packer versions): the outputs name the file (frame names,
// the sheet's source, the clips' source.path), so a copy or a rename imports again; a touched
// file hits its cache; deleting the cache only costs a reimport. A project without .aseprite files starts no job and makes no directory.
//
// Thread model: attach/update/reimport/record on the main thread; runImport on any thread;
// the provider (called by pool loads on job threads) reads a snapshot under the module's mutex.
// A new version swaps in only once its pages are loaded, so sprites never show an empty frame.

#pragma once

#include "pixelart/PixelProblem.h"
#include "pixelart/aseprite/AsepriteImport.h"

#include "asset/Guid.h"
#include "jobs/Task.h"

#include "json.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {
    class Texture;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart::aseprite {

    // One import, as the cache holds it
    struct ImportRecord {
        struct Clip {
            std::string key;          // "animation:walk"
            std::string name;         // the reference's clip part
            std::string file;         // "clip-0.anim"
            float length = 0.0f;
            std::string wrap;
        };
        struct Output {
            std::string key;
            std::string name;
            std::string kind;         // "frame", "image", "animation", "slice", "tileset", "palette"
            Guid guid;
        };

        std::string assetPath;        // asset-relative
        Guid sourceGuid;
        std::string key;              // the cache key k
        std::filesystem::path dir;    // the cache directory of k
        bool ok = false;
        PixelProblem error;
        std::vector<PixelProblem> warnings;
        int frames = 0;
        std::vector<std::pair<int, int>> pages;   // sizes
        std::vector<Clip> clips;
        std::vector<Output> outputs;
        std::vector<std::string> tags, slices;
        nlohmann::json settings;      // as resolved (MCP form)
        std::string summary;          // the inspector's line
        double ms = 0.0;
        bool cacheHit = false;
        uint64_t sequence = 0;        // its request's (adopt drops an import older than the one shown)

        std::filesystem::path sheetFile() const { return dir / "sheet.json"; }
        std::filesystem::path pageFile(size_t page) const { return dir / ("page" + std::to_string(page) + ".png"); }
        // {sheet, pages, clips: [{name, key, reference, guid, length, wrap}], slices, sub_assets, warnings, cache}
        nlohmann::json toJson() const;
        const Clip* findClip(const std::string& keyOrName) const;

        // What an export ships of the import, the one table the editor's provider and the
        // export's index.json both come from: each `<source>#<key>` with its cache file and its
        // asset-relative path in an export (je-imported/<k[0:2]>/<k>/...)
        struct ExportedFile {
            std::string key;              // "sheet", "image#0", "animation:walk"
            std::filesystem::path cacheFile;
            std::string exportPath;
        };
        std::vector<ExportedFile> exportedFiles() const;
        // the cache file of a key (the provider's answer): "sheet", "image#<n>", "animation:<tag>"
        // or a clip's name; false when the import has none
        bool resolveKey(const std::string& key, std::filesystem::path& file) const;
    };

    // What an import needs, gathered on the main thread
    struct ImportRequest {
        std::filesystem::path file;               // absolute
        std::string assetPath;
        Guid sourceGuid;
        AsepriteImportSettings settings;
        int projectPixelsPerUnit = 1;
        std::filesystem::path cacheRoot;          // .jasperengine/cache/imported/v1
        std::map<std::string, Guid> namedGuids;   // the asset database's persisted sub-asset GUIDs
        uint64_t sequence = 0;                    // the order requests were made in (the watcher's and the importer's own)
    };

    class AsepriteImporter {
    public:
        static AsepriteImporter& get();

        void attach(Project* project);
        Project* getProject() const { return project; }

        // main thread, every loop: adopts finished imports; true when something changed
        bool update();

        // R23 and settings edits: a new import of the file (asset-relative or absolute), queued
        void reimport(const std::string& path);
        // every AsepriteSprite asset the database knows (project open, a full sync)
        void requestAll();
        // until the files' imports are in (export, MCP writes, tests); false on timeout
        bool waitFor(const std::vector<std::string>& assetPaths, int timeoutMs);
        // true when the file has an import and none is running (main thread); else starts one if
        // there is none and returns false: callers wait without blocking (an MCP call's waitUntil,
        // whenImported)
        bool ensureImport(const std::string& assetPath);
        // `then` on the main thread once the file's running (or next) import has finished, failed
        // or not; at once when it has an import and none is running
        void whenImported(const std::string& assetPath, std::function<void()> then);

        // the current (adopted) import of a file, null before its first one
        std::shared_ptr<const ImportRecord> record(const std::string& assetPath) const;
        // the outputs (sheet, clip references, GUIDs) of a file: R23's interface for S45/S47
        nlohmann::json outputs(const std::string& assetPath) const;
        bool isPending(const std::string& assetPath) const;
        // the latest import that finished, failed ones too (record() keeps the last good one), and
        // how many have finished for the file: a caller that changed the settings waits for the
        // count to move and reads the outcome of its own import
        std::shared_ptr<const ImportRecord> lastAttempt(const std::string& assetPath) const;
        uint64_t attemptCount(const std::string& assetPath) const;
        // S45: told (main thread) when a file's new import is swapped in (Sprite Atlases rebuild);
        // one listener per owner
        void addImportListener(const void* owner, std::function<void(const std::string& assetPath)> listener);
        void removeImportListener(const void* owner);
        // imports started since attach (R25: a project without .aseprite files starts none)
        uint64_t jobsStarted() const { return started; }

        // the settings of a file (its meta; defaults when absent) and the request of an import
        AsepriteImportSettings settingsOf(const std::string& assetPath, std::vector<PixelProblem>* problems = nullptr) const;
        std::optional<ImportRequest> requestOf(const std::string& assetPath) const;
        std::filesystem::path cacheRoot() const;
        static constexpr const char* kSettingsKey = "import.asepriteImporter";

        // The import itself (any thread): a cache hit reads import.json, else read, compose, pack
        // and write. A file that cannot be read throws (the hot reload service retries it); every
        // other failure is a record with ok = false.
        static std::shared_ptr<ImportRecord> runImport(const ImportRequest& request);
        // the cache key of a file's bytes and request
        static std::string cacheKey(const std::string& contentSha256, const ImportRequest& request);
        // swaps a finished import in (main thread): pools invalidated once its pages are loaded
        void adopt(std::shared_ptr<ImportRecord> record);
        // the ImportedAssets answer for a reference (any thread)
        bool resolve(const std::string& sourceRef, std::string& fileOut) const;

    private:
        AsepriteImporter() = default;

        struct Swap {
            std::shared_ptr<ImportRecord> record;
            std::vector<std::shared_ptr<Texture>> pages;   // loading, kept alive until the swap
        };

        Project* project = nullptr;
        mutable std::mutex mutex;   // records (the provider reads them from job threads)
        std::map<std::string, std::shared_ptr<const ImportRecord>> records;
        std::map<std::string, std::shared_ptr<const ImportRecord>> attempts;
        std::map<std::string, uint64_t> attemptCounts;
        std::map<std::string, uint64_t> adoptedSequences;   // the newest import adopted per file
        std::map<std::string, jobs::Task<std::shared_ptr<ImportRecord>>> running;
        std::set<std::string> again;   // asked again while running
        std::vector<std::pair<std::string, std::function<void()>>> waiting;   // whenImported
        std::vector<Swap> swaps;
        std::map<const void*, std::function<void(const std::string&)>> importListeners;
        uint64_t started = 0;

        void start(const std::string& assetPath);
        void finishSwap(Swap& swap);
        std::string assetPathOf(const std::string& path) const;
    };

}
