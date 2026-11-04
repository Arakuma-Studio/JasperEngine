// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's model cooks (spec S53 R13): every model of the project is cooked for the desktop
// family on S06's Background lane, the selected ones first. Edit mode and Play load a model's
// cooked desktop result once it is ready (CookedModels' provider, MeshSystem's one redirect) and
// its source until then: a cook that lands reloads the models showing it, as a changed file
// does (S05). A source change, a sidecar write (SetAssetMetaCmd, undo included, or another
// program's) or a project setting cancels the model's cooks and queues them again; a cook whose
// source changed since it was made is never offered.
//
// Paths are asset-relative ("models/rock.glb").
//
// ModelBatchCook is the blocking form the export and MCP's reimport use.

#pragma once

#include "model/ModelCooker.h"
#include "model/ModelUsage.h"

#include "jobs/JobSystem.h"
#include "model/CookedModels.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor {

    class Project;

    enum class ModelCookStatus : uint8_t {
        None,       // not planned yet
        Queued,
        Cooking,
        Cooked,
        Failed,
        Source,     // not a cookable model (OBJ): loads from its source
    };
    const char* modelCookStatusName(ModelCookStatus status);   // "queued", "cooked"...

    // The model files of the cook (.gltf, .glb, .fbx)
    bool isModelSource(const std::string& path);

    struct ModelCookState {
        ModelCookStatus status = ModelCookStatus::None;
        // the artifact shown is older than the source or the settings (a recook is queued)
        bool stale = false;
        bool cacheHit = false;
        std::string error;
        std::string key;
        ResolvedModelSettings resolved;
        ModelCookRecord record;             // Cooked
        // the file cooked (an FBX's import) as it was cooked: a changed file never gets the old cook
        std::filesystem::path sourceFile;
        uint64_t sourceSize = 0;
        int64_t sourceTime = 0;
    };

    // Every model source of the project, asset-relative
    std::vector<std::string> projectModelSources(const Project& project);
    // The settings of a model as stored (none: the defaults); false with the reason when malformed
    bool modelSettingsOf(const Project& project, const std::string& assetPath, ModelImportSettings& out, std::string* error = nullptr);
    // What a cook of one model for one family reads, gathered on the main thread: its settings
    // (or `override`, a dry run's), the project's defaults, the submeshes scenes draw blended,
    // and the file to read (an FBX's import, made now when missing)
    bool gatherModelCookRequest(Project& project, const std::string& assetPath, ModelFamily family, const ModelImportSettings* override,
                                ModelCookRequest& out, std::string& error);

    class ModelCookQueue {
    public:
        static ModelCookQueue& get();

        // Binds the queue to an open project; null closes it (running cooks are waited for)
        void attach(Project* project);
        Project* getProject() const { return project; }

        // Once a loop iteration, main thread: starts queued cooks, adopts finished ones, reloads
        // what shows a model whose cook landed. True while it has work.
        bool update();

        enum Priority : int { Background = 0, Loaded = 2, Selected = 3 };
        void request(const std::string& assetPath, ModelFamily family, int priority = Background);
        void requestAll(const std::vector<ModelFamily>& families, int priority = Background);
        // the model's cooks are cancelled, marked stale and queued again
        void invalidate(const std::string& assetPath);
        void invalidateAll();
        // the S05 watcher: a model source changed on disk (a no-op when it still is what was cooked)
        void sourceChanged(const std::string& assetPath);
        // a sidecar write: reloads the model's settings, invalidates on change
        void settingsChanged(const std::string& assetPath);
        // the status bar's cancel
        void cancelAll();

        bool getState(const std::string& assetPath, ModelFamily family, ModelCookState& out) const;
        bool isPending(const std::string& assetPath, ModelFamily family) const;
        // the worst state over the families it was queued for (Project window badge)
        ModelCookStatus badgeStatus(const std::string& assetPath) const;

        struct Progress {
            int done = 0;           // since the queue was last idle
            int total = 0;
            std::string current;    // a model cooking now
        };
        Progress getProgress() const;
        bool isBusy() const;

        // Blocks (pumping update()) until every cook of these paths x families is terminal, or
        // the timeout passes. Main thread. False on timeout.
        bool waitFor(const std::vector<std::string>& assetPaths, const std::vector<ModelFamily>& families, double timeoutSeconds);

        ModelCookCache& getCache() { return cache; }
        // Project Settings > Mesh changed: the cache cap and every model's key
        void projectSettingsChanged();

        // the families an export platform needs: desktop, mobile (Android, iOS), web
        static std::vector<ModelFamily> familiesForExport(const std::vector<std::string>& platforms);

    private:
        struct Job;
        struct Entry {
            ModelCookState state;
            int priority = Background;
            uint64_t epoch = 0;
            bool queued = false;
            uint64_t order = 0;
            std::shared_ptr<Job> job;
        };

        ModelCookQueue();

        Project* project = nullptr;
        ModelCookCache cache;
        mutable std::mutex mutex;
        std::map<std::pair<std::string, int>, Entry> entries;
        std::vector<std::shared_ptr<Job>> running;
        std::vector<std::string> reloads;          // landed cooks of the shown family: reload their models
        uint64_t sequence = 0;
        int progressDone = 0;
        int progressTotal = 0;
        bool scanned = false;
        std::chrono::steady_clock::time_point lastUsageRefresh;

        bool cookedEntry(const std::string& assetPath, CookedModelEntry& out);
        void startJobs();
        void collectJobs();
        void reloadShown();
        void cancelJobs(bool wait);
        std::filesystem::path absoluteOf(const std::string& assetPath) const;
        std::string assetPathOf(const std::filesystem::path& absolute) const;
    };

    // One model x family of a batch
    struct ModelBatchItem {
        std::string assetPath;
        ModelFamily family = ModelFamily::Desktop;
        ModelCookRequest request;
        ModelCookResult result;
        bool sourceOnly = false;    // not a cookable model: nothing to cook
    };

    class ModelBatchCook {
    public:
        // Cooks every item through `cache` on up to `threads` models at once (0: the job system's
        // worker count). `progress` gets (done, total, path) after each; `cancelled` stops the
        // batch (the rest reports cancelled). Requests come from gatherModelCookRequest.
        static void run(ModelCookCache& cache, std::vector<ModelBatchItem>& items, int threads,
                        const std::function<void(int, int, const std::string&)>& progress, const std::function<bool()>& cancelled);
    };

}
