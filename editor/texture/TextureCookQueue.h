// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's texture cooks (spec S14 R8): every texture of the project is planned and cooked
// for the desktop family on S06's Background lane, the selected texture and the textures the
// open scenes load first. A cook a family view renders with (desktop, or the Game view's
// simulated family, R26) is swapped in place when it lands; until then the texture renders
// through the raw path with its import processing. An S05 source change or a settings edit
// cancels the texture's running cooks and queues them again. Concurrent cooks share a
// working-memory budget (default 2 GB), so a few 8K sources cannot exhaust RAM.
//
// Paths are asset-relative ("textures/rock.png"), as the texture pool names them.
//
// TextureBatchCook is the blocking form the export, the CLI and MCP's reimport use: a set of
// textures and families, cooked through the same cache with a thread count.

#pragma once

#include "texture/TextureCooker.h"
#include "texture/TextureTypeInference.h"

#include "jobs/JobSystem.h"
#include "texture/CookedTextures.h"
#include "texture/TexturePrepare.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor {

    class Project;

    enum class TextureCookStatus : uint8_t {
        None,       // not planned yet
        Queued,
        Cooking,
        Cooked,
        Failed,
        Source,     // read from its source file only (terrain layers, HTML images): not cooked
    };
    const char* textureCookStatusName(TextureCookStatus status);   // "queued", "cooked", "source"...

    // The image formats a cook reads (R1, without .svg and .ktx2, which load as they are)
    bool isCookableTexture(const std::string& path);
    // R1's texture sources (the cookable ones, .svg and .ktx2)
    bool isTextureSource(const std::string& path);

    struct TextureCookState {
        TextureCookStatus status = TextureCookStatus::None;
        // the artifact shown is older than the source or the settings (a recook is queued)
        bool stale = false;
        bool cacheHit = false;
        std::string error;
        TextureCookPlan plan;           // once planned
        TextureCookRecord record;       // Cooked
        std::vector<std::string> warnings;   // R2 conflicts, R6, clamping
        std::vector<std::string> problems;   // the warning-level ones: R2 conflicts, R6 (`--check`)
    };

    // What a cook of one texture reads: its settings and what its references say (R2, R3)
    struct TextureCookInputs {
        std::string assetPath;
        std::string projectRelative;
        std::filesystem::path absolute;
        TextureImportSettings settings;
        bool hasSettings = false;       // false: the settings were inferred now (to be written)
        TextureInference inference;
    };

    // What a cook of one texture reads: its sidecar settings (inferred when it has none) and its
    // references. Main thread (the index's overlay of unsaved documents).
    bool gatherTextureInputs(Project& project, const std::string& assetPath, TextureCookInputs& out);
    TextureCookRequest makeTextureCookRequest(const TextureCookInputs& inputs, const TextureProjectSettings& project, TextureFamily family);
    // The project's settings with its TextureStrategy (a cook input, R31)
    TextureProjectSettings textureProjectSettingsOf(const Project& project);
    // Every texture source of the project, asset-relative (cookable ones only when asked)
    std::vector<std::string> projectTextureSources(const Project& project, bool cookableOnly);

    // One texture × family of a batch
    struct TextureBatchItem {
        std::string assetPath;
        TextureFamily family = TextureFamily::Desktop;
        TextureCookResult result;
        bool sourceOnly = false;        // nothing to cook (result.ok, no record)
    };

    class TextureCookQueue {
    public:
        static TextureCookQueue& get();

        // Binds the queue to an open project (its cache, settings and references); null closes
        // it: running cooks are cancelled and waited for
        void attach(Project* project);
        Project* getProject() const { return project; }
        bool isAttached() const { return project != nullptr; }

        // Once a loop iteration, main thread: starts planned cooks within the budget, adopts the
        // finished ones, swaps cooked textures in. True while it has work (the loop stays awake).
        bool update();

        // Queues (or raises the priority of) a texture's cook for a family
        enum Priority : int { Background = 0, Loaded = 2, Selected = 3 };
        void request(const std::string& assetPath, TextureFamily family, int priority = Background);
        // every texture of the project, for these families (Reimport All Textures, the export
        // window's Cook Textures Now)
        void requestAll(const std::vector<TextureFamily>& families, int priority = Background);
        // A source or settings change: the texture's cooks are cancelled, marked stale and queued
        // again; what renders it reloads (raw now, cooked when the new cook lands)
        void invalidate(const std::string& assetPath);
        void invalidateAll();
        // The S05 file watcher: a texture source changed on disk
        void sourceChanged(const std::string& assetPath);
        // A sidecar write (SetAssetMetaCmd): reloads the texture's settings, invalidates on change
        void settingsChanged(const std::string& assetPath);
        // stops every queued and running cook (the status bar's cancel)
        void cancelAll();

        bool getState(const std::string& assetPath, TextureFamily family, TextureCookState& out) const;
        // queued or cooking (a cooked texture being cooked again keeps its Cooked state, stale)
        bool isPending(const std::string& assetPath, TextureFamily family) const;
        // the worst state of a texture over the families it was queued for (Project window badge)
        TextureCookStatus badgeStatus(const std::string& assetPath) const;

        struct Progress {
            int done = 0;       // since the queue was last idle
            int total = 0;
            std::string current;    // a texture cooking now
        };
        Progress getProgress() const;
        bool isBusy() const;

        // Blocks (pumping update()) until every cook of these paths × families is terminal, or
        // the timeout passes. Main thread. False on timeout.
        bool waitFor(const std::vector<std::string>& assetPaths, const std::vector<TextureFamily>& families, double timeoutSeconds);

        // The family the editor renders with (desktop; the Game view's Simulate platform
        // textures picks another): the cooked-texture provider answers for it
        void setViewFamily(TextureFamily family);
        TextureFamily getViewFamily() const;

        // the settings of a texture as stored (or inferred when it has none); any thread
        bool settingsOf(const std::string& assetPath, TextureImportSettings& out);

        TextureCookCache& getCache() { return cache; }
        void setMemoryBudget(uint64_t bytes);
        uint64_t getMemoryBudget() const;
        // Project Settings > Texture changed: the cache cap and dir, and every texture's key
        void projectSettingsChanged();
        TextureProjectSettings getProjectSettings() const;

        // the families an export platform needs (R9)
        static std::vector<TextureFamily> familiesForExport(const std::vector<std::string>& platforms, bool androidEtc2Fallback);

    private:
        struct Job;
        struct Entry {
            TextureCookState state;
            int priority = Background;
            uint64_t epoch = 0;             // bumped by invalidate: a finished job of an older epoch is dropped
            bool queued = false;
            uint64_t order = 0;
            std::shared_ptr<Job> job;
        };
        struct SwapIn;

        TextureCookQueue();
        ~TextureCookQueue();

        Project* project = nullptr;
        TextureCookCache cache;
        TextureProjectSettings projectSettings;
        uint64_t memoryBudget = 2ull << 30;
        uint64_t memoryInUse = 0;
        std::atomic<int> viewFamily{(int)TextureFamily::Desktop};

        mutable std::mutex mutex;           // entries, settings memo, inbox
        std::map<std::pair<std::string, int>, Entry> entries;
        // the stored settings of a texture (first: it has some)
        std::unordered_map<std::string, std::pair<bool, TextureImportSettings>> settingsMemo;
        std::vector<std::string> loaderInbox;   // loaded through the raw path: cook at Loaded priority
        std::vector<std::shared_ptr<Job>> running;
        std::vector<std::shared_ptr<SwapIn>> swaps;
        std::deque<std::pair<std::string, std::string>> metaWrites;   // assetPath, YAML of inferred settings
        uint64_t sequence = 0;              // request order among equal priorities
        int progressDone = 0;
        int progressTotal = 0;
        bool scanned = false;               // the whole project queued once the index was ready

        bool cookedEntry(const std::string& assetPath, CookedTextureEntry& out);
        bool processingFor(const std::string& assetPath, TextureProcessing& out);
        void startJobs();
        void collectJobs();
        void startSwap(const std::string& assetPath);
        void collectSwaps();
        void writeInferredSettings();
        void scanProject();
        std::filesystem::path absoluteOf(const std::string& assetPath) const;
        std::string assetPathOf(const std::filesystem::path& absolute) const;
        // a file the editor derived into .jasperengine (an .aseprite import's pages): its owner decides
        // how it is compressed, so the view never cooks it
        bool isEditorDerived(const std::string& assetPath) const;
        void cancelJobs(bool wait);
    };

    class TextureBatchCook {
    public:
        // Cooks every item through `cache` on up to `threads` textures at once (0: the job
        // system's worker count), within a working-memory budget. `progress` gets (done, total,
        // path) after each item; `cancelled` stops the batch (the rest reports cancelled).
        // `inputs` comes from gatherTextureInputs (main thread).
        static void run(TextureCookCache& cache, const TextureProjectSettings& project,
                        const std::map<std::string, TextureCookInputs>& inputs, std::vector<TextureBatchItem>& items,
                        int threads, uint64_t memoryBudget, const std::function<void(int, int, const std::string&)>& progress,
                        const std::function<bool()>& cancelled);
    };

}
