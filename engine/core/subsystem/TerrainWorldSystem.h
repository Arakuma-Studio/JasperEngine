// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_SYSTEM_H
#define TERRAIN_WORLD_SYSTEM_H

// T4: the scene's Terrain Worlds. A world's runtime is made for each GameObject with a Terrain and a
// Terrain World, pumped once a frame from Engine::runFrame after the job pump, paused or not (D9),
// and released with the component. Its update fires onAreaReady in the update phase (R18). A scene
// without a world costs one empty-array check a frame (R37).

#include "Export.h"
#include "ecs/Entity.h"
#include "ecs/SubSystem.h"
#include "subsystem/TerrainWorldRuntime.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    struct TerrainWorldComponent;
    namespace terrain {
        class FoliageRegion;
    }

    class JASPERENGINE_API TerrainWorldSystem : public SubSystem {
    public:
        // Where a world's data comes from and whether its scene plays. A player has none: the pack's
        // CookedWorldSource, playing. The editor sets its own (editor/terrainworld/).
        struct Host {
            std::function<std::shared_ptr<TerrainWorldSource>(Scene* scene, Entity entity, const TerrainWorldComponent& world)> makeSource;
            std::function<bool(Scene* scene)> playing;
            // after a runtime started: the editor gives it layer colours and its focus
            std::function<void(Scene* scene, Entity entity, TerrainWorldRuntime& runtime)> started;
            // before a runtime goes: the editor saves its edited tiles
            std::function<void(Scene* scene, Entity entity, TerrainWorldRuntime& runtime)> released;
        };
        static void setHost(Host host);

        explicit TerrainWorldSystem(Scene* scene);
        ~TerrainWorldSystem() override;

        void load() override;
        void destroy() override;
        void update(double dt) override;
        void draw() override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // D9: every scene's worlds, once a frame (later calls in the same frame do nothing)
        static void pumpAll();
        // tests: pumpAll does nothing while held, as for frames closer than its dedupe; pump still runs
        static void setPumpHeldForTesting(bool held);
        // Every live runtime of every scene (the editor's flush, its panel); main thread
        static void forEachRuntime(const std::function<void(TerrainWorldRuntime& runtime)>& fn);
        void pump(double now);

        TerrainWorldRuntime* getRuntime(Entity entity);
        // A world's runtime, made now if its component is there and it has none yet (the editor and
        // MCP ask before the first pump)
        TerrainWorldRuntime* ensureRuntime(Entity entity, std::string* error = nullptr);
        bool hasWorlds() const { return !runtimes.empty(); }
        // Why a world could not start (its data missing or refused), for the inspector and MCP
        std::string getError(Entity entity) const;
        // The world's data changed under it (a new asset, convert, import): made again at the next pump
        void restart(Entity entity);

        // R18: called once in the update phase when the area is ready; a paused game gets it on resume
        int onAreaReady(Entity entity, float x, float z, float radius, TerrainWorldReadyLevel level, std::function<void()> callback);
        void cancelAreaReady(int id);

        // R35: the profiler scope's name
        static constexpr const char* PROFILE_SCOPE = "TerrainWorld";

    private:
        struct AreaCallback {
            int id = 0;
            Entity entity = NULL_ENTITY;
            int wait = 0;
            float x = 0.0f, z = 0.0f, radius = 0.0f;
            TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
            std::function<void()> callback;
        };

        // R27: T5's foliage, one region per resident tile
        struct FoliageTile {
            std::shared_ptr<terrain::FoliageRegion> region;
            uint32_t revision = 0;
        };

        void syncRuntimes();
        void updateFocuses(TerrainWorldRuntime& runtime, const TerrainWorldComponent& component);
        void syncFoliage(Entity entity, TerrainWorldRuntime& runtime);
        void releaseFoliage(Entity entity);
        void release(Entity entity);

        std::map<Entity, std::unique_ptr<TerrainWorldRuntime>> runtimes;
        std::map<Entity, std::map<uint32_t, FoliageTile>> foliageTiles;
        std::map<Entity, std::string> errors;
        std::vector<AreaCallback> areaCallbacks;
        int nextCallbackId = 1;
        double lastPump = -1.0;
    };

}

#endif // TERRAIN_WORLD_SYSTEM_H
