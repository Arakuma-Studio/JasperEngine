// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_SYSTEM_H
#define FOLIAGE_SYSTEM_H

// A Terrain's foliage at runtime (T5): placing details on jobs, the tree store and its cells, what a
// view draws, the runtime overlay and the queries. Pure work lives in engine/core/terrain/. The system
// holds what is resolved from a Terrain, so the authored component stays copiable. A scene with no
// Terrain costs one empty-array check per frame (R38).

#include "SubSystem.h"

#include "buffer/ExternalBuffer.h"
#include "component/InstancedMeshComponent.h"
#include "ecs/ComponentFwd.h"
#include "ecs/Entity.h"
#include "jobs/Task.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "object/TerrainEdit.h"
#include "terrain/FoliageCull.h"
#include "terrain/FoliageScatter.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TreeStore.h"
#include "util/Signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine {

    namespace terrain {

        // F11, the seam for T4: a lone Terrain is one region, a tiled world is one per resident tile
        class FoliageRegion {
        public:
            virtual ~FoliageRegion() = default;
            virtual uint32_t regionKey() const = 0;
            virtual TerrainMapStore& maps() = 0;                        // T1: density, height, holes (D4)
            virtual const TerrainSurfaceQuery& surface() const = 0;     // T1 (D5)
            virtual std::shared_ptr<TreeStore> trees() = 0;             // may be null while loading
            virtual void cellOffset(int& x, int& z) const = 0;          // whole 8-unit cells from the world origin
            virtual Matrix4 localToWorld() const = 0;

            // A region with a frame of its own (a world's tile, T4 R27): a square of size units centred on
            // localToWorld's origin, heights over maxHeight, a height map spanning heightMapSize (its
            // samples may reach past the square), trees from trees() alone. False: the owner Terrain's
            // component holds them, as a lone Terrain's does.
            struct Frame {
                float size = 0.0f;
                float heightMapSize = 0.0f;
                float maxHeight = 0.0f;
                bool treesLoading = false;
            };
            virtual bool frame(Frame& out) const {
                (void)out;
                return false;
            }
        };

    }

    // What sample_terrain, the Stats foldout and the budgets read (T5 R26, R50, R55)
    struct FoliageStats {
        uint64_t detailResident = 0;
        uint64_t detailVisible = 0;
        uint64_t treesTotal = 0;
        uint64_t treesResident = 0;
        uint64_t treesVisible = 0;
        uint64_t meshTrees = 0;
        uint32_t draws = 0;
        uint32_t cells = 0;
        uint32_t pendingCells = 0;
        uint32_t detailCellsPlaced = 0;     // since load: what a partial rebuild keeps small
        uint32_t treeCellsRebuilt = 0;
        uint32_t meshLoads = 0;
        uint32_t applied = 0;               // cells applied on the last update
        uint64_t streamBytes = 0;           // all views, last built
        uint64_t uploadBytes = 0;           // last frame
        uint64_t bufferBytes = 0;           // tree cells on the GPU
        uint64_t cpuBytes = 0;
        uint64_t gpuBytes = 0;
        double mainThreadMs = 0.0;          // last frame, within R5's budget
        double budgetSpentMs = 0.0;
        bool budgetExceeded = false;
    };

    struct FoliageHit {
        Entity terrain = NULL_ENTITY;
        terrain::TreeId id = 0;
        int prototype = -1;         // index in treePrototypes; -1 for a detail
        int layer = -1;             // detail layer
        float distance = 0.0f;
        Vector3 point;
    };

    struct FoliageInstanceInfo {
        terrain::TreeId id = 0;     // 0 for a detail
        bool tree = true;
        int prototype = -1;
        int layer = -1;
        Vector3 position;           // world
        Quaternion rotation;
        float scale = 1.0f;
    };

    struct TreeHitEvent {
        terrain::TreeId id = 0;
        int prototype = -1;
        Vector3 point;
        Vector3 normal;
        Entity body = NULL_ENTITY;
        float speed = 0.0f;
    };

    // One instanced draw of a prototype in a view: a slice of a buffer at one level of detail
    struct FoliageDrawItem {
        BufferRender* buffer = nullptr;
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t level = 0;
    };

    // What a view draws: its stream and each prototype's draws (T5 F4, F5, F13)
    struct FoliageViewList {
        std::vector<terrain::FoliageInstance> stream;
        ExternalBuffer buffer;
        size_t capacity = 0;
        // R37: the layers whose vertex program has no compact variant, as 96-byte instances
        std::vector<InstanceRenderData> expanded;
        ExternalBuffer expandedBuffer;
        size_t expandedCapacity = 0;
        bool needUpload = false;

        std::vector<FoliageDrawItem> items;
        // by InstancedMeshComponent::foliageSlot: first item and count
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        uint64_t signature = 0;
        bool built = false;

        // the last build's counts
        uint64_t detailVisible = 0;
        uint64_t treesVisible = 0;
        uint64_t meshTrees = 0;
        uint32_t draws = 0;
        bool capped = false;

        // working memory kept between builds
        std::vector<terrain::FoliageInstance> scratch;
        std::vector<uint8_t> scratchLods;
    };

    struct FoliageViewRequest {
        const Plane* planes = nullptr;
        float farClip = 0.0f;
        Vector3 eye;            // for distance: a shadow view passes the main camera's
        bool lodEnabled = false;
        Vector3 lodOrigin;
        float lodThreshold = 1.0f;
        float lodPerUnit = 0.0f;
        float lodConstant = 0.0f;
        bool shadow = false;
        bool main = false;      // its counts are the stats
    };

    class JASPERENGINE_API FoliageSystem : public SubSystem {
    public:
        static constexpr double APPLY_BUDGET_MS = 0.5;              // R5
        static constexpr float RESNAP_DEBOUNCE_SECONDS = 0.12f;     // R25
        static constexpr int MAX_RUNTIME_DISCS = 64;                // F10
        static constexpr int MAX_PREVIEW_INSTANCES = 2000000;       // T5 risks

        explicit FoliageSystem(Scene* scene);
        ~FoliageSystem() override;

        void load() override;
        void destroy() override;
        void update(double dt) override;
        void draw() override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // MeshSystem's public foliage functions forward here (R6)
        bool setPreviewEntity(Entity entity);
        bool hasPendingUpdates() const;
        std::vector<Entity> getEntities(Entity terrainEntity) const;
        Entity getOwner(Entity foliageEntity) const;
        void reloadMesh(const std::string& filename);
        // Exact over resident cells: the instances the placement holds for this layer
        uint64_t getInstanceCount(Entity terrainEntity, int layer) const;
        // why the layer places nothing (a map without CPU-readable pixels), or empty
        std::string getLayerError(Entity terrainEntity, int layer) const;

        // F11. A lone Terrain gets its region by itself. T4 adds one per resident tile.
        void addRegion(Entity owner, std::shared_ptr<terrain::FoliageRegion> region);
        void removeRegion(Entity owner, uint32_t regionKey);
        // A region's maps changed under it: its cells are placed again and its trees re-snap, after the
        // re-snap debounce, as an edit's are (T4 R27: a tile written by an edit)
        void regionChanged(Entity owner, uint32_t regionKey);
        size_t getRegionCount(Entity owner) const;
        // The tree stores of a Terrain's regions that have one (a world's resident tiles, T4 R27)
        void forEachTreeStore(Entity owner, const std::function<void(const terrain::TreeStore& store)>& visit) const;

        // ---- trees

        // Null while loading, or when the file was refused
        std::shared_ptr<terrain::TreeStore> getTreeStore(Entity terrainEntity);
        std::shared_ptr<const terrain::TreeStore> getTreeStore(Entity terrainEntity) const;
        // A store the Terrain did not have: a new one for painting, kept until the caller saves it
        std::shared_ptr<terrain::TreeStore> getOrCreateTreeStore(Entity terrainEntity);
        // R22: a bad file: the message, and whether a save would lose what is in it
        bool isTreeStoreRefused(Entity terrainEntity, std::string* error = nullptr) const;
        void allowOverwriteRefusedStore(Entity terrainEntity);
        bool isTreeStoreLoading(Entity terrainEntity) const;
        // Written since the last clearTreeStoreDirty (what the editor flushes with the maps)
        bool isTreeStoreDirty(Entity terrainEntity) const;
        void clearTreeStoreDirty(Entity terrainEntity);
        // Runtime scene clones share the immutable store, each with its own overlay (R62)
        void shareTreeStore(Entity terrainEntity, std::shared_ptr<terrain::TreeStore> store);

        // After a write to the store: only those records' cells (R26)
        void treesChanged(Entity terrainEntity, const terrain::TreeDelta& delta);
        // Everything may have changed; a new store may come with it
        void treeStoreReplaced(Entity terrainEntity, std::shared_ptr<terrain::TreeStore> store = nullptr);
        // The file now holds the store (a save, by whoever wrote it)
        void treeFileWritten(Entity terrainEntity, const std::string& path);

        // ---- runtime overlay (R30, R31)

        std::optional<FoliageInstanceInfo> removeTreeInstance(Entity terrainEntity, terrain::TreeId id, bool updateNavigation = false);
        bool restoreTreeInstance(Entity terrainEntity, terrain::TreeId id);
        // World position; 0 when the prototype does not exist
        terrain::TreeId addTreeInstance(Entity terrainEntity, int prototype, const Vector3& position, float yawDegrees, float scale);
        void clearDetailsInRadius(Entity terrainEntity, const Vector3& position, float radius, int layer = -1);
        void restoreDetails(Entity terrainEntity);
        std::string getRuntimeState(Entity terrainEntity) const;
        bool setRuntimeState(Entity terrainEntity, const std::string& state);
        // Drops the overlay (a reloaded scene starts clean)
        void clearRuntimeState(Entity terrainEntity);

        Signal<Entity, terrain::TreeId, const FoliageInstanceInfo&> onTreeRemoved;
        Signal<Entity, const TreeHitEvent&> onTreeHit;
        // One handler per Terrain, Lua's terrain.onTreeHit and onTreeRemoved; held until the scene goes
        void setTreeHitHandler(Entity terrainEntity, std::function<void(const TreeHitEvent&)> handler);
        void setTreeRemovedHandler(Entity terrainEntity, std::function<void(terrain::TreeId, const FoliageInstanceInfo&)> handler);

        // ---- queries (R32, R35)

        // A hit's body and colliderIndex to its tree (Contact3D.h, RayReturn); false for any other body
        bool getTreeFromBody(Entity bodyEntity, size_t colliderIndex, Entity& terrainEntity, terrain::TreeId& id) const;
        bool isTreeBody(Entity bodyEntity) const;

        // Nearest first; kind 0 trees, 1 details, 2 all
        std::vector<FoliageInstanceInfo> getInstancesInRadius(Entity terrainEntity, const Vector3& position, float radius, int kind = 0, size_t maxResults = 256) const;
        // The world pose and record of a tree, stored or added at runtime; false when it is unknown or removed
        bool getTreePose(Entity terrainEntity, terrain::TreeId id, FoliageInstanceInfo& out) const;
        // A cell walk along the ray: bounding spheres; the caller tests triangles. Each candidate, nearest first
        void forEachTreeAlongRay(const Ray& ray, float maxDistance, const std::function<bool(Entity terrain, terrain::TreeId id, int prototype, const Matrix4& instanceWorld, float sphereDistance)>& visit) const;
        void forEachDetailAlongRay(const Ray& ray, float maxDistance, const std::function<bool(Entity terrain, int layer, const Matrix4& instanceWorld, float sphereDistance)>& visit) const;
        // Local bounds of a prototype's LOD0; false until it is loaded
        bool getPrototypeBounds(Entity terrainEntity, int prototype, Vector3& center, float& radius) const;
        Entity getPrototypeEntity(Entity terrainEntity, int prototype, int lod = 0) const;
        Entity getDetailEntity(Entity terrainEntity, int layer) const;

        // Every visible tree of the prototypes that have navObstacle: world position and radius, height
        void forEachNavObstacle(Entity terrainEntity, const std::function<void(const Vector3& base, float radius, float height)>& visit) const;
        // The resident tree cells: what collision builds from
        void forEachResidentCell(Entity terrainEntity, const std::function<void(int cellX, int cellZ, std::span<const terrain::FoliageInstance> instances, std::span<const uint32_t> ids)>& visit) const;

        // ---- stats

        const FoliageStats& getStats(Entity terrainEntity) const;
        // R2's canonical hash over the resident cells (layer, cell Z, cell X, attempt)
        uint64_t getPlacementHash(Entity terrainEntity) const;
        // What any view's lists depend on, beyond the camera: a cached shadow reads it, too (R17)
        uint32_t getRevision() const { return residencyRevision; }
        // Waits for what jobs hold: the next update applies it (a test, a capture that must not wait for frames)
        void waitForJobs();
        // The wind a Terrain's plants feel now, world, its length the strength (R20)
        Vector3 getWindVector(const TerrainComponent& terrain, double timeSeconds) const;

        // ---- what RenderSystem calls (F1's hooks)

        // True when any Terrain has something to draw: the render hooks' first check
        bool hasDrawables() const { return drawables; }
        // Whether the plants of a Terrain move: wind with some bend and some speed (R19)
        bool isWindBlowing(Entity terrainEntity) const;
        void buildView(FoliageViewList& list, const FoliageViewRequest& request);
        // Tree cell buffers, within the per-frame cap (R14)
        void uploadTreeBuffers();
        void setLastUploadBytes(uint64_t bytes);

        // The draw parameters of a prototype entity (u_vs_foliage)
        struct PrototypeDraw {
            Entity terrain = NULL_ENTITY;
            int prototype = -1;
            int layer = -1;
            bool impostor = false;
            float inStart = 0.0f, inEnd = 0.0f;             // 0 before start, 1 at end (the impostor's fade in)
            float outStart = 0.0f, outEnd = 0.0f;           // 1 before start, 0 at end
            bool colorMask = false;                         // red channel masks the flutter
            Vector3 wind;                                   // world, this frame's
            float heightStart = 0.0f, heightRange = 1.0f;   // the mesh's own units, where sway starts and is full
            Vector3 impostorCenter;
            Vector2 impostorSize;
            float impostorViews = 0.0f;
            bool impostorBlend = true;
        };
        bool getPrototypeDraw(Entity prototypeEntity, PrototypeDraw& out) const;

    private:
        void rebuildCellNow(Entity terrainEntity, uint32_t regionKey, const terrain::TreeRecord& record);
        void updateContactSubscription();
        void reportMemory(uint64_t cpuBytes, uint64_t gpuBytes);
        uint64_t reportedCpuBytes = 0;
        uint64_t reportedGpuBytes = 0;
        uint64_t loadedSignature = 0;
        bool contactsConnected = false;

        struct Impl;
        std::unique_ptr<Impl> impl;
        bool drawables = false;
        uint32_t residencyRevision = 0;
        uint32_t nextSlot = 0;
        friend struct Impl;
    };

}

#endif // FOLIAGE_SYSTEM_H
