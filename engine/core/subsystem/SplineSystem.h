// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_SYSTEM_H
#define SPLINE_SYSTEM_H

#include "SubSystem.h"

#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/SplineAnimateComponent.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "spline/SplineStation.h"
#include "spline/SplineStations.h"
#include "spline/SplineVertex.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    struct TerrainComponent;
    struct Transform;

    // What a generator found wrong (the Extrude and Instantiate inspectors, build_spline; S18
    // R24). knot is the knot the warning concerns, -1 for the whole component.
    enum class SplineWarningKind{
        MitreLimit,         // a corner turned past 120 degrees: built Round
        InnerFold,          // the inside of a turn folds over itself (radius too wide)
        CornersOverlap,     // two corners' rings overlap
        WaterlineRise,      // WaterBanks: the waterline had to rise to close the banks
        ConformMiss,        // stations off the terrain held the nearest height
        InstanceCap,        // more pieces than the cap: the rest are not placed
        Triangulation,      // Fill: the outline crosses or touches itself
        NonUniformScale,    // the GameObject's scale is not uniform: radius uses its largest axis
        NoTerrain,          // conform or WaterBanks without a terrain under the spline
        PoolEmpty,          // an Instantiate pool without a loadable entry
        PrefabsNotInEditor, // prefab pieces are GameObjects in Play only
        OverHole            // stations over a terrain hole: a road bridges it at the height map's height (T3 R26)
    };

    struct JASPERENGINE_API SplineWarning{
        SplineWarningKind kind = SplineWarningKind::MitreLimit;
        int knot = -1;
        std::string message;
    };

    // What a consumer last built (build_spline, the inspector's readout)
    struct JASPERENGINE_API SplineBuildStats{
        uint64_t generation = 0;            // the build these numbers belong to
        uint32_t stations = 0;
        uint32_t vertices = 0;
        uint32_t triangles = 0;
        uint32_t chunks = 0;
        uint32_t chunksRebuilt = 0;         // of chunks, the ones the last change rebuilt
        uint32_t instances = 0;
        float length = 0.0f;                // world units, draped
        double buildMs = 0.0;
        bool pending = false;               // a rebuild is scheduled or running
    };

    // One piece a Spline Instantiate placed: the editor's "Bake to GameObjects" makes it a GameObject
    struct JASPERENGINE_API SplinePiecePlacement{
        std::string path;                   // the model or prefab
        Vector3 position;
        Quaternion rotation;
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
    };

    // The water surface of a spline river or lake under a point (Water Volume buoyancy; S22)
    struct JASPERENGINE_API SplineWaterSample{
        bool hit = false;
        float height = 0.0f;                // world Y of the surface
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 flow;                       // world units per second along the river
        Entity owner = NULL_ENTITY;         // the GameObject with the Spline Extrude
    };

    // Regenerates what is built on splines (S18 R30, D3): Spline Extrude meshes in chunks,
    // Spline Instantiate pieces (instanced models, prefab GameObjects in Play) and the colliders,
    // whenever a spline, a consumer's settings, the GameObject's transform or the terrain under a
    // conformed consumer changes; and moves the Spline Animate followers. Generated chunks are
    // engine-owned GameObjects of the scene, never saved and never in the hierarchy (the
    // foliage precedent). Below 2 ms of estimated work a rebuild runs inline in the frame; above
    // it, its chunks are jobs of the Background lane (time-sliced by the web's single-thread
    // pump), debounced to 120 ms after the last change while the editor drags. A scene with no
    // spline components costs one empty-array check per frame (R21).
    class JASPERENGINE_API SplineSystem : public SubSystem {
    public:
        // a rebuild estimated below this runs in the frame (R30)
        static constexpr double INLINE_BUDGET_MS = 2.0;
        // while the editor drags, a large rebuild waits this long after the last change
        static constexpr double DRAG_DEBOUNCE_MS = 120.0;
        // chunk length along the curve and the vertex limit of a chunk (16-bit indices, R27)
        static constexpr float CHUNK_LENGTH = 64.0f;
        static constexpr uint32_t CHUNK_MAX_VERTICES = 65535;
        // instances of one instanced chunk, prefab GameObjects of one component
        static constexpr uint32_t CHUNK_MAX_INSTANCES = 8192;
        static constexpr uint32_t MAX_PREFAB_INSTANCES = 512;
        // pieces of one component (Spacing, Tile and Fence)
        static constexpr uint32_t MAX_PIECES = 100000;

    private:
        struct ExtrudeChunk;
        struct PieceChunk;
        struct PoolModel;
        struct Resolve;
        struct BuildJob;

        std::unordered_map<Entity, std::unique_ptr<Resolve>> resolves;
        // generated GameObject -> its owner (picking, the editor's lists)
        std::unordered_map<Entity, Entity> generatedOwners;

        double clockMs = 0.0;
        bool interactiveEdit = false;
        bool liveBuilds = true;

        Resolve& resolveOf(Entity entity);
        void release(Entity entity);
        void releaseResolve(Resolve& resolve);

        void updateConsumers(Entity entity, SplineComponent& spline, Transform& transform);
        void updateAnimate(double dt);
        void dropPiecesOverHoles(const SplineComponent& spline, BuildJob& job);

        bool collectStations(Entity entity, const SplineComponent& spline, const Transform& transform,
                             spline::SplineStationOptions& options, std::vector<spline::SplineStation>& stations,
                             std::vector<SplineWarning>& warnings);

        void scheduleExtrude(Entity entity, Resolve& resolve, const SplineComponent& spline, const Transform& transform,
                             const SplineExtrudeComponent& extrude);
        void finishExtrude(Entity entity, Resolve& resolve);
        void applyExtrudeChunks(Entity entity, Resolve& resolve, BuildJob& job);

        void scheduleInstantiate(Entity entity, Resolve& resolve, const SplineComponent& spline, const Transform& transform,
                                 const SplineInstantiateComponent& instantiate);
        bool preparePools(Entity entity, Resolve& resolve, const SplineInstantiateComponent& instantiate);
        void applyPieces(Entity entity, Resolve& resolve, BuildJob& job);

        void updateCollision(Entity entity, Resolve& resolve, const SplineExtrudeComponent* extrude);
        void syncChunkMaterials(Entity entity, Resolve& resolve);

        Entity createChunkEntity(Entity owner);
        void destroyGenerated(Entity entity);
        void destroyPrefabPiece(Entity root);

        TerrainComponent* findTerrainUnder(const Vector3& worldPoint, Entity& terrainEntity) const;

    public:
        SplineSystem(Scene* scene);
        virtual ~SplineSystem();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentRemoved(Entity entity, ComponentId componentId) override;
        // paused (the editor out of Play): Spline Animate holds still; generation goes on. A
        // change places prefab pieces again (they are GameObjects in Play only).
        void setPaused(bool value) override;

        // the generated GameObjects of a spline (chunks, pieces, colliders), and the owner of one
        std::vector<Entity> getGeneratedEntities(Entity owner) const;
        Entity getGeneratedOwner(Entity generated) const;
        bool isGenerated(Entity entity) const;
        // the world bounds of everything generated for a spline (picking the road selects it)
        AABB getGeneratedBounds(Entity owner) const;

        // the warnings and statistics of the last build of a consumer
        std::vector<SplineWarning> getExtrudeWarnings(Entity owner) const;
        std::vector<SplineWarning> getInstantiateWarnings(Entity owner) const;
        SplineBuildStats getExtrudeStats(Entity owner) const;
        SplineBuildStats getInstantiateStats(Entity owner) const;
        // every piece of the last Spline Instantiate build, world space, in placement order
        std::vector<SplinePiecePlacement> getPiecePlacements(Entity owner) const;

        // Rebuilds a spline's consumers now and waits for them (MCP build_spline, tests, the
        // "Bake to GameObjects" button): no debounce, no background lane.
        void rebuildNow(Entity owner);
        // whether a rebuild of this spline is scheduled or running
        bool isRebuildPending(Entity owner) const;

        // The world stations of a spline (S18 D5): the draped, conformed polyline every consumer
        // and the terrain bake share. spacing <= 0: the Extrude's (or 1). False without a Spline.
        bool getWorldStations(Entity owner, float spacing, std::vector<spline::SplineStation>& stations, bool conform = true);

        // The water surface of the spline rivers and lakes (Extrude WaterBanks, or Flat with
        // flow) under a world XZ; the highest one where several overlap.
        SplineWaterSample sampleWaterSurface(float x, float z, Entity owner = NULL_ENTITY) const;
        bool hasWaterSurfaces() const;
        // the spline of this GameObject has a water surface (its Water Volume follows it)
        bool hasWaterSurface(Entity owner) const;

        // The editor drags knots: large rebuilds wait DRAG_DEBOUNCE_MS after the last change
        void setInteractiveEdit(bool interactive);
        bool isInteractiveEdit() const;
        // false: nothing regenerates until it is true again (the editor's batch edits)
        void setLiveBuilds(bool live);

        // the milliseconds splines have worked since the start, on every thread: the updates, and
        // the chunk builds and layouts run as jobs (the web's per-frame budget, R17)
        static double getWorkMs();

        // Spline Animate
        void play(Entity follower);
        void pause(Entity follower);
        void restart(Entity follower);
        // the follower's position for a distance along its spline (world units, wrapped/clamped)
        void setFollowerDistance(Entity follower, float distance);
    };

}

#endif //SPLINE_SYSTEM_H
