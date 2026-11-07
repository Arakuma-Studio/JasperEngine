// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_SYSTEM_H
#define VOXEL_SYSTEM_H

// T6: everything a Voxel Volume needs from the scene. Chunks are engine-owned GameObjects, never
// saved and never in the hierarchy (R19), a chunk's mesh is built on a job and goes on screen only as
// part of a whole change set (R17), and collision swaps follow the render activation (R23). A scene
// with no volume costs one empty-array check per frame (R58).

#include "SubSystem.h"

#include "ecs/ComponentFwd.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "voxel/VoxelChangeSet.h"
#include "voxel/VoxelClipbox.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelFile.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelStore.h"
#include "voxel/VoxelWorkQueue.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    // The tier decides what a volume costs (R51). Stored in MeshComponent::voxelTier.
    enum class VoxelTier : uint8_t{ High = 1, Medium = 2, Low = 3 };

    struct VoxelTierSettings{
        int lodLevels = 4;
        int collisionRadius = 3;
        int64_t uploadBytesPerFrame = 16 * 1024 * 1024;
        int uploadMeshesPerFrame = 32;
        int bodyCap = 512;
        int bodyBatch = 32;
        int rescueBodiesPerTick = 64;
        int maxEditVoxels = 2 * 1000 * 1000;
        int shadowLevels = 4;     // how many levels cast (R52)
    };
    JASPERENGINE_API VoxelTierSettings voxelTierSettings(VoxelTier tier);

    // R7: per target, Project Settings > Voxels can lower them
    struct VoxelLimits{
        int maxDimension = 1024;
        int64_t cpuBudget = 256 * 1024 * 1024;
        int64_t meshBudget = 128 * 1024 * 1024;
        VoxelTier autoTier = VoxelTier::High;
    };
    JASPERENGINE_API VoxelLimits voxelPlatformLimits();
    JASPERENGINE_API void setVoxelLimitsOverride(const VoxelLimits& limits);
    JASPERENGINE_API VoxelLimits voxelActiveLimits();

    // A .voxelvolume in memory. The editor's scenes share one per path (asset semantics, R-Risks); Play
    // and players clone the store, so no Play edit reaches the editor's data (R6).
    struct VoxelAsset{
        std::string path;
        std::shared_ptr<voxel::Store> store;
        std::shared_ptr<const std::vector<uint8_t>> fileBytes;
        uint64_t fileHash = 0;
        int slotCount = 0;
        voxel::GeneratorDesc generator;
        voxel::TerrainCutRuns terrainCut;
        uint32_t fileFlags = 0;
        bool dirty = false;
        bool baseResolved = false;
        // a failed load keeps the file untouched and refuses to overwrite it (R61)
        voxel::FileError error = voxel::FileError::None;
        std::string errorMessage;
    };

    // How the module finds the base of a generator kind it cannot build alone: a Terrain, a Noise Graph.
    // Null: the kind stays unresolved, and base_stale says so.
    using VoxelBaseResolver = std::function<std::shared_ptr<const voxel::BaseSource>(const voxel::GeneratorDesc&, const voxel::VolumeDesc&)>;

    class JASPERENGINE_API VoxelAssets{
    public:
        // One per path while anyone holds it; loaded on first use
        static std::shared_ptr<VoxelAsset> acquire(const std::string& path);
        // From bytes already in hand (tests, MCP create). keep may be null when the caller owns them.
        static std::shared_ptr<VoxelAsset> fromBytes(const std::string& path, std::shared_ptr<const std::vector<uint8_t>> bytes);
        static std::shared_ptr<VoxelAsset> create(const std::string& path, const voxel::VolumeDesc& desc, const voxel::GeneratorDesc& generator, int slotCount);
        // Forgets the path: the next acquire reads the file again (S05's external change)
        static void forget(const std::string& path);
        static void setBaseResolver(VoxelBaseResolver resolver);
        // Builds the store's base from its generator, if it is built-in or the resolver knows it
        static void resolveBase(VoxelAsset& asset);
        // R7's budget over all volumes
        static std::shared_ptr<voxel::MemoryBudget> sharedBudget();
    };

    struct VoxelStats{
        voxel::Store::BlockCounts blocks;
        voxel::MemoryStats memory;
        int64_t meshBytes = 0;
        int64_t triangles = 0;
        int chunks = 0;
        int draws = 0;
        int jobsPending = 0;
        int collisionBodies = 0;
        double lastMeshMs = 0.0;
        int lastLatencyFrames = -1;
        // this frame's uploads, for R18
        int64_t uploadBytesThisFrame = 0;
        int uploadMeshesThisFrame = 0;
        int rescues = 0;
        // queue jobs that ran on the main thread: 0 with workers (AC20)
        int shapesBuiltOnMain = 0;
        VoxelTier tier = VoxelTier::High;
        int levels = 1;
    };

    class JASPERENGINE_API VoxelSystem : public SubSystem{
    public:
        explicit VoxelSystem(Scene* scene);
        ~VoxelSystem() override;

        void load() override;
        void destroy() override;
        void update(double dt) override;
        void fixedUpdate(double dt) override;
        void draw() override;
        void setPaused(bool value);

        // ---- chunks (R19)
        Entity getGeneratedOwner(Entity entity) const;
        std::vector<Entity> getGeneratedEntities(Entity owner) const;
        AABB getGeneratedBounds(Entity owner) const;

        // ---- loading (R20): false until the volume's first LOD set is active
        bool isVolumeReady(Entity owner) const;
        // For RenderSystem::forEachLoad: every volume not yet on screen
        void forEachLoading(const std::function<void(Entity)>& visit) const;

        // ---- data
        voxel::Store* getStore(Entity owner);
        std::shared_ptr<VoxelAsset> getAsset(Entity owner) const;
        // the loaded file's error, or "" (R61)
        std::string getError(Entity owner) const;
        // world <-> the volume's frame, metres
        bool worldToLocal(Entity owner, const Vector3& world, voxel::Vec3d& local) const;
        Vector3 localToWorld(Entity owner, const voxel::Vec3d& local) const;
        voxel::Quatd localRotationOf(Entity owner, const Quaternion& worldRotation) const;
        Matrix4 modelMatrixOf(Entity owner) const;

        // ---- editing (R26-R31). Shapes in the volume's frame.
        voxel::EditResult edit(Entity owner, std::span<const voxel::EditOp> ops, voxel::EditOptions options);
        // Undo, redo, regenerate, a revert: blocks changed behind the mesher's back
        void notifyChanged(voxel::Store* store, const voxel::IntBox& changed);
        void applyDelta(Entity owner, const std::vector<voxel::SlotChange>& delta, bool undo);
        // The whole volume changed: asset swapped, generator regenerated
        void reloadVolume(Entity owner);

        // R35: a base over a Terrain, through a copy a job may hold; null if the entity has no Terrain.
        std::shared_ptr<const voxel::BaseSource> makeTerrainBaseFor(Entity owner, Entity terrain);
        // ---- runtime saves (R79)
        bool saveChanges(Entity owner, const std::string& slot, std::string& error);
        bool loadChanges(Entity owner, const std::string& slot, std::string& error);
        bool hasSavedChanges(Entity owner, const std::string& slot) const;
        void revertChanges(Entity owner);

        // ---- physics (R21-R24; VoxelSystemPhysics.cpp)
        void ensureCollision(Entity owner, const AABB& worldBounds);
        bool isCollisionReady(Entity owner, const AABB& worldBounds) const;

        // ---- navigation (R25): the level-0 collision geometry inside the box, in world space
        void appendNavTriangles(const AABB& worldBounds, std::vector<Vector3>& vertices, std::vector<uint32_t>& indices);

        // ---- stats (R64)
        VoxelStats getStats(Entity owner) const;
        // The meshes on screen, for the audit (V6) and the overlays
        std::vector<std::shared_ptr<const voxel::MeshOut>> getVisibleMeshes(Entity owner) const;
        std::map<voxel::BlockKey, voxel::MeshId> getVisibleBlocks(Entity owner) const;
        // For tests: run everything queued to the end, on this thread where it can
        void finishPendingWork();
        // Tiers can change at runtime (Auto follows the terrain's)
        VoxelTier resolveTier(const VoxelVolumeComponent& component) const;

    private:
        struct VolumeState;
        struct Job;
        friend struct VolumeState;

        VolumeState* stateOf(Entity owner);
        const VolumeState* stateOf(Entity owner) const;
        void syncVolumes();
        bool ensureLoaded(Entity owner, VoxelVolumeComponent& component, VolumeState& state);
        void removeState(Entity owner);
        void updateVolume(VolumeState& state, VoxelVolumeComponent& component, double dt);
        void updateFocus(VolumeState& state, VoxelVolumeComponent& component);
        void flushRequests(VolumeState& state);
        void pumpJobs();
        void uploadChunks(VolumeState& state, const VoxelVolumeComponent& component);
        void pollActivations(VolumeState& state);
        Entity acquireChunk(VolumeState& state, const VoxelVolumeComponent& component);
        void releaseChunk(VolumeState& state, Entity chunk);
        void writeChunk(VolumeState& state, const VoxelVolumeComponent& component, Entity chunk, const voxel::MeshId& id, const voxel::MeshOut& mesh);
        void applyOwnerTransform(VolumeState& state, Entity chunk);
        void fireEvents(VolumeState& state, VoxelVolumeComponent& component);
        void updateNavigation(VolumeState& state, VoxelVolumeComponent& component, double dt);
        Vector3 focusPositionWorld(const VoxelVolumeComponent& component) const;
        voxel::Vec3d worldVoxels(const VolumeState& state, const Vector3& world) const;

        // VoxelSystemPhysics.cpp
        void updateCollisionWanted(VolumeState& state, VoxelVolumeComponent& component);
        void startCollisionJob(VolumeState& state, const voxel::Int3& key, bool partOfSwap);
        void applyCollisionSwaps(VolumeState& state);
        void addNewBodies(VolumeState& state);
        void evictBodies(VolumeState& state);
        void rescueBodies(VolumeState& state, VoxelVolumeComponent& component, double dt);
        void onCollisionEdited(VolumeState& state, const voxel::IntBox& changed, uint64_t setId);
        void onCollisionJobDone(VolumeState& state, Job& job);
        void destroyCollision(VolumeState& state);
        void buildCollisionBlocking(VolumeState& state, const VoxelVolumeComponent& component, const voxel::IntBox& blocks);

        // VoxelSystemTerrain.cpp
        void attachTerrain(VolumeState& state, VoxelVolumeComponent& component);
        void updateTerrainCut(VolumeState& state, VoxelVolumeComponent& component, const voxel::IntBox& changed);
        void detachTerrain(VolumeState& state, VoxelVolumeComponent& component);

        std::unordered_map<Entity, std::unique_ptr<VolumeState>> states;
        std::unordered_map<Entity, Entity> generatedOwners;
        std::map<uint64_t, std::unique_ptr<Job>> jobs;
        std::unique_ptr<voxel::WorkQueue> queue;
        uint64_t nextJobId = 1;
        uint64_t editIndex = 0;
        uint64_t frameCounter = 0;
        uint64_t fixedTick = 0;
    };

}

#endif
