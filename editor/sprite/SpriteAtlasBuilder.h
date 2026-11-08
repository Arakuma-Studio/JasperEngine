// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite Atlas builds (docs/specs/S45-sprite-atlas.md R14-R19, R27, §6.6): an atlas's members
// (textures with their Sprite Mode, imported .aseprite files) packed by S44's SpritePacker into
// lossless pages, the atlas's sheet and, for each member, its view into the pages (the remap the
// pool draws a packed source from), on a Background job.
//
//   .jasperengine/cache/atlas/v1/<atlas guid>/desktop/page<n>-<hash>.png   the editor's pages (named by their bytes)
//   .jasperengine/cache/atlas/v1/<atlas guid>/export-<family>/...   an export's own build of a family
//   .../member-<hash of path>.json    a member's remapped jasperengine.spritesheet record
//   .../sheet.json                    the atlas's own sheet
//   .../build.json                    stats, frames, warnings and the input key (written last)
//
// Incremental: a member's frames are cached by its content stamp (a texture's and its meta's
// size and time, an import's cache key), so unchanged members are not read again; a build whose
// input key (the atlas text, the platform, every member's stamp, the versions) matches build.json
// is adopted without packing; pages are named by their bytes, so a page whose bytes did not change
// is neither written nor cooked again.
// The editor's ImportedAssets sheet provider answers "<atlas>#sheet", "<atlas>#image#<n>" and, for
// the members of the atlases in use (mode Enabled, Include in Build on), "<member>#sheet",
// "<member>#image#<n>", "<member>#atlas"; then texture sheets (SpriteSheetBuilder).
// Thread model: gather/attach/update on the main thread; build on any thread; resolve from any.

#pragma once

#include "SpriteAtlasDocument.h"
#include "SpritePacker.h"

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include "asset/Guid.h"
#include "jobs/Primitives.h"
#include "jobs/Task.h"
#include "sprite/SpriteSheet.h"

#include "json.hpp"

#include <atomic>
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

namespace jasperengine::editor::sprite {

    constexpr int kAtlasBuilderVersion = 1;

    // R18
    enum class AtlasMode { Enabled, EnabledForBuilds, Disabled };
    const char* atlasModeName(AtlasMode mode);      // "enabled", "enabled_for_builds", "disabled"
    AtlasMode atlasModeFromName(const std::string& name);

    struct AtlasMemberInput {
        std::string path;                 // asset-relative
        std::filesystem::path absolute;
        std::string kind;                 // "texture" | "aseprite_sprite"
        std::string stamp;                // its content identity
        std::filesystem::path sheetFile;  // an .aseprite file's import: its sheet and pages
        std::vector<std::filesystem::path> pageFiles;
        bool pixelArt = false;            // lossless by nature (Auto compression, Auto filter)
    };

    struct AtlasBuildInput {
        std::string atlasPath;            // asset-relative
        SpriteAtlasDocument doc;
        std::string platform = "desktop"; // the family whose max size applies
        std::vector<AtlasMemberInput> members;    // by path
        std::vector<pixelart::PixelProblem> problems;   // missing members, members still importing
        std::filesystem::path outDir;     // "" packs in memory only (a preview)
        std::string compression;          // resolved: lossless | platform_default
        std::string filter;               // resolved: point | bilinear
        bool forceBlockAlign = false;     // tests: a block format whatever the compression

        std::string key() const;          // the build's input key
    };

    // One member's frames as the atlas packs them (cached by stamp)
    struct AtlasMemberFrames {
        std::string path, kind, stamp;
        SpriteSheetData sheet;            // its own sheet (names, pivots, borders, tags...)
        std::vector<std::shared_ptr<const pixelart::PixelImage>> images;   // the untrimmed frames, sheet order
        pixelart::PixelProblem error;
    };

    struct AtlasFrameRow {
        std::string name, member, memberFrame;
        int page = 0;
        pixelart::IntRect rect;           // in the page
        pixelart::IntSize sourceSize;
        pixelart::IntPoint offset;        // trimmed from (the trim's top-left in the source frame)
        int waste = 0;                    // transparent pixels trimmed away
        int duplicateOf = -1;             // the frame (atlas index) whose pixels this one shares
    };

    struct AtlasPageRow {
        int width = 0, height = 0;
        int frames = 0;
        double used = 0.0;                // trimmed frame area / page area
        std::string file;                 // its name in outDir ("page0-<hash>.png")
        std::string hash;
        bool written = false;             // this build wrote it (its bytes are new)
    };

    struct AtlasBuildResult {
        bool ok = false;
        pixelart::PixelProblem error;
        std::vector<pixelart::PixelProblem> warnings;
        std::string atlasPath, platform, key, compression, filter;
        Guid guid;
        std::filesystem::path dir;
        SpriteSheetData sheet;            // the atlas's own sheet
        std::vector<AtlasPageRow> pages;
        std::vector<AtlasFrameRow> frames;
        std::vector<std::string> members;               // packed, by path
        std::map<std::string, std::string> memberFiles; // member -> its remap's file name in dir
        std::map<std::string, int> memberPages;         // member -> its first page
        std::vector<std::pair<std::string, int>> worstMembers;   // largest transparent area before trim
        int duplicates = 0;
        double usage = 0.0;
        double ms = 0.0;
        bool cacheHit = false;
        bool relayout = true;             // the layout changed (member set or a frame size)
        std::vector<pixelart::PixelImage> pageImages;   // in-memory builds (previews)

        std::filesystem::path pageFile(size_t page) const { return dir / pages[page].file; }
        std::filesystem::path sheetFile() const { return dir / "sheet.json"; }
        std::filesystem::path memberFile(const std::string& member) const;
        // VRAM bytes of the pages on a family (lossless: RGBA8; platform formats: 1 byte a pixel):
        // "web" counts the web layout when its max size differs (webPages)
        size_t vramBytes(const std::string& platform) const;
        std::vector<std::pair<int, int>> webPages;      // the web family's page sizes, when its layout differs
        // the inspector's stats line ("3 pages · 1,204 frames · 87 % used · ...")
        std::string statsLine() const;
        // what get_sprite_atlas returns (frames filtered by search: name substring, member:, page:)
        nlohmann::json toJson(const std::string& search, int limit, int page, bool withFrames) const;

        // the files an export ships ("sheet", "image#<n>", and each member's remap)
        struct ExportedFile {
            std::string source;           // the "<source>" of the reference
            std::string key;              // "sheet", "image#0", "atlas"
            std::filesystem::path cacheFile;
            std::string exportPath;       // je-imported/atlas/<guid>/<platform>/...
        };
        std::vector<ExportedFile> exportedFiles(bool remaps) const;
    };

    struct AtlasProgress {
        std::atomic<int> done{0}, total{0};
    };

    class SpriteAtlasBuilder {
    public:
        static SpriteAtlasBuilder& get();

        void attach(Project* project);
        Project* getProject() const { return project; }
        // main thread, every loop: adopts finished builds; true when something changed
        bool update();

        // every atlas of the project (project open, a mode change)
        void requestAll();
        // a build of the atlas (asset-relative or absolute), queued behind a running one
        void rebuild(const std::string& atlasPath);
        // a member's pixels or settings changed: the atlases that pack it build again
        void memberChanged(const std::string& memberPath);
        // an atlas file went away: its build is dropped and its members draw their sources again;
        // its cache folder goes too unless it was renamed (removeCache false: the GUID's folder is reused)
        void forget(const std::string& atlasPath, bool removeCache = true);
        // the project's Sprite Atlas mode changed
        void modeChanged();
        AtlasMode mode() const;

        bool isBuilding(const std::string& atlasPath) const;
        // the pages written of its running build (false when none runs)
        bool progressOf(const std::string& atlasPath, int& done, int& total) const;
        bool isBusy() const { return !running.empty(); }
        // the status bar's "Building sprite atlas ui (2/3 pages)"; "" when idle
        std::string statusText() const;
        void cancelAll();
        // until the atlases are built (export, MCP, tests); false on timeout
        bool waitFor(const std::vector<std::string>& atlasPaths, int timeoutMs);
        uint64_t buildCount(const std::string& atlasPath) const;

        // the last good build (what the editor draws), and the last one that finished, failed too
        std::shared_ptr<const AtlasBuildResult> result(const std::string& atlasPath) const;
        std::shared_ptr<const AtlasBuildResult> lastAttempt(const std::string& atlasPath) const;
        // every atlas file of the project (asset-relative, sorted)
        std::vector<std::string> atlasPaths() const;
        // the atlases in use packing a member (sorted; the first wins), and its page there
        std::vector<std::string> atlasesPacking(const std::string& memberPath) const;
        std::string packedIn(const std::string& memberPath, int* page = nullptr) const;
        // double packing and other cross-atlas warnings of an atlas
        std::vector<pixelart::PixelProblem> crossWarnings(const std::string& atlasPath) const;

        // ImportedAssets' sheet provider (any thread)
        bool resolve(const std::string& sourceRef, std::string& fileOut) const;

        // ---- the build, callable on its own (export, MCP preview, tests)
        // main thread: the members of an atlas resolved, their stamps and import files; false
        // with *error when the atlas file does not read
        static bool gather(Project& project, const std::string& atlasPath, const SpriteAtlasDocument& doc, const std::string& platform,
                           AtlasBuildInput& out);
        // any thread
        std::shared_ptr<AtlasBuildResult> build(const AtlasBuildInput& input, AtlasProgress* progress = nullptr,
                                                const jobs::CancelToken* cancel = nullptr);
        // a member's frames (cached by its stamp)
        std::shared_ptr<const AtlasMemberFrames> memberFrames(const AtlasMemberInput& member);
        // the atlas document of a file; nullopt (with the problem) when it does not read
        static std::optional<SpriteAtlasDocument> readDocument(const std::filesystem::path& file, pixelart::PixelProblem* problem);
        // where an export builds a family: its own folder beside the editor's (an export never
        // writes or prunes the pages the editor draws)
        static std::filesystem::path exportDir(const AtlasBuildInput& input);
        // removes the page and remap files of the build's folder that neither this build nor the
        // folder's build.json (the newest build written there) names
        static void removeStaleFiles(const AtlasBuildResult& result);
        std::filesystem::path cacheRoot() const;
        static std::filesystem::path cacheRootOf(const Project& project);

    private:
        SpriteAtlasBuilder() = default;

        struct Running {
            jobs::Task<std::shared_ptr<AtlasBuildResult>> task;
            std::shared_ptr<AtlasProgress> progress;
            std::shared_ptr<jobs::CancelToken> cancel;
        };

        Project* project = nullptr;
        mutable std::mutex mutex;   // results, remaps, member cache (the provider and jobs read them)
        std::map<std::string, std::shared_ptr<const AtlasBuildResult>> results;
        std::map<std::string, std::shared_ptr<const AtlasBuildResult>> attempts;
        // the atlas's include_in_build at its last build (an atlas in use)
        std::map<std::string, bool> included;
        struct Swap {
            std::shared_ptr<AtlasBuildResult> result;
            std::vector<std::shared_ptr<Texture>> pages;   // loading, kept alive until the swap
        };
        std::vector<Swap> swaps;
        std::map<std::string, std::string> remaps;           // member -> the atlas in use that wins
        std::map<std::string, std::vector<std::string>> packers;   // member -> atlases in use packing it
        std::map<std::string, std::shared_ptr<const AtlasMemberFrames>> memberCache;
        std::map<std::string, uint64_t> builds;
        std::map<std::string, Running> running;
        std::set<std::string> again;
        std::set<std::string> waitingImports;   // atlases waiting for a member's import
        AtlasMode currentMode = AtlasMode::Enabled;

        void start(const std::string& atlasPath);
        void adopt(const std::shared_ptr<AtlasBuildResult>& result);
        void finishSwap(Swap& swap);
        void refreshRemaps();
        // removes the cache folders of atlases that are gone, and the families nothing builds any more
        void pruneCache();
        std::string assetPathOf(const std::string& path) const;
    };

}
