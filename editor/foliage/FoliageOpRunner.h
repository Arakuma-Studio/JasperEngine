// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Runs a list of T5's terrain operations from the editor (R23, R48, R54): on a snapshot of the maps
// it reads, off the main thread when a job system is there, committed on the main thread as one undo
// step. Mass Place and edit_terrain both come through here, so a dry run and a real run are one path.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "terrain/FoliageOps.h"
#include "terrain/RawMapStore.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    class Project;

    class FoliageOpRunner {
    public:
        // A world point's place on a Terrain, in its local space (what ops' shapes are in)
        static Vector3 toTerrainLocal(Scene* scene, Entity terrain, const Vector3& world);

        struct Request {
            uint32_t sceneId = 0;
            Entity terrain = NULL_ENTITY;
            std::vector<terraincore::Op> ops;
            bool dryRun = false;
            std::string undoName;   // "Scatter Foliage (Island)"
        };

        struct Outcome {
            terraincore::TerrainOpResult result;
            int64_t treesBefore = 0;
            int64_t treesAfter = 0;
            bool cancelled = false;
            std::string error;
        };

        using Finished = std::function<void(const Outcome&)>;
        using Progress = std::function<void(double share)>;

        // The T1 store's bytes for what a list reads, then the list, on a job; its end posts the
        // landing to the editor's main-thread queue (updateAll).
        static std::shared_ptr<FoliageOpRunner> start(Project* project, Request request, Finished finished, Progress progress = nullptr);

        // Everything a running job needs from the main thread; false once finished
        bool poll();
        void cancel() { stop.store(true); }
        bool isFinished() const { return finished; }

        // Lands every runner whose work is done: posted to the editor's main-thread queue by the work
        static void updateAll();

        // Fills a host: prototypes by mesh stem, detail layers, splines, instanced hosts
        static void fillHost(Project* project, uint32_t sceneId, Entity terrain, terraincore::FoliageOpHost& host);

    private:
        struct Work;
        std::shared_ptr<Work> work;
        Project* project = nullptr;
        Request request;
        Finished onFinished;
        Progress onProgress;
        std::atomic<bool> stop{false};
        bool finished = false;
        uint64_t storeRevision = 0;

        void commit();
        void commitWorld(Outcome& outcome);
        // the maps and trees a list reads: one Terrain's, or each tile's of a World (T4 R27)
        void planTerrain(Work& work);
        void planWorld(Work& work);
    };

}
