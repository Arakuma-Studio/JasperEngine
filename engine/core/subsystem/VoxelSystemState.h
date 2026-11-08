// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_SYSTEM_STATE_H
#define VOXEL_SYSTEM_STATE_H

// What VoxelSystem keeps per volume, shared by its three source files

#include "subsystem/VoxelSystem.h"
#include "component/VoxelVolumeComponent.h"

#include "terrain/TerrainMapStore.h"
#include "util/Signal.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyID.h"
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/Shape/Shape.h"
#endif

#include <array>
#include <deque>
#include <map>
#include <tuple>
#include <unordered_map>

namespace jasperengine{

    inline voxel::Vec3d toVec3d(const Vector3& v){
        return voxel::Vec3d{v.x, v.y, v.z};
    }

    struct VoxelSystem::Job{
        enum Kind{ Mesh, Collision } kind = Mesh;
        Entity owner = NULL_ENTITY;
        voxel::MeshId id;
        std::shared_ptr<const voxel::ReadView> view;
        std::shared_ptr<voxel::MeshOut> mesh;
        std::shared_ptr<const voxel::Coverage> coverage;
        voxel::Int3 collisionKey;
        double buildMs = 0.0;
        bool partOfSwap = false;
        uint32_t collisionVersion = 0;   // the block's wanted version when the job started
#ifdef JASPERENGINE_PHYSICS_3D
        JPH::ShapeRefC shape;
        bool empty = false;
        bool builtOnMainThread = false;
#endif
    };

    struct VoxelCollisionBlock{
        Entity entity = NULL_ENTITY;
#ifdef JASPERENGINE_PHYSICS_3D
        JPH::BodyID body;
        JPH::ShapeRefC pendingShape;
#endif
        bool inWorld = false;
        // Created and prepared, not finalized: Jolt lets nothing else touch it yet (V8)
        bool adding = false;
        bool building = false;
        bool built = false;          // the job finished at this version
        bool pendingEmpty = false;
        uint64_t lastNeededTick = 0;
        // Versions of the edits: wanted is the last one reaching the block, version the one its built
        // shape holds, applied the one in the world. Two swaps over one block apply its shape once:
        // the second finds it applied and leaves the body be.
        uint32_t version = 0;
        uint32_t wantedVersion = 0;
        uint32_t appliedVersion = 0;
    };

    struct VoxelCollisionSwap{
        uint64_t renderSet = 0;
        std::vector<voxel::Int3> blocks;
        voxel::IntBox changed;
        uint64_t editFrame = 0;
    };

    struct VoxelSystem::VolumeState{
        Entity owner = NULL_ENTITY;
        std::shared_ptr<VoxelAsset> asset;
        std::string assetPath;
        // the asset's store while editing, a clone in Play and players (R6)
        std::shared_ptr<voxel::Store> store;
        bool ownsStore = false;
        voxel::ChangeSetScheduler scheduler;
        voxel::Clipbox clipbox;
        VoxelTier tier = VoxelTier::High;
        int lodRadius = 4;
        int levels = 1;
        float maxDraw = 0.0f;
        bool wantedDraws = true;
        bool firstSetRequested = false;
        bool firstSetActive = false;
        bool warnedMoved = false;
        std::string error;
        bool loggedError = false;

        Matrix4 model;
        Matrix4 inverseModel;
        Vector3 worldPosition;
        Quaternion worldRotation;

        // chunks by mesh; empty meshes have none (R19)
        std::map<voxel::MeshId, Entity> chunks;
        std::vector<Entity> chunkPool;
        std::deque<std::pair<voxel::MeshId, std::shared_ptr<const voxel::MeshOut>>> uploadQueue;
        std::vector<voxel::MeshId> uploading;
        std::map<voxel::MeshId, std::shared_ptr<const voxel::MeshOut>> onScreen;
        std::map<voxel::MeshId, uint64_t> pendingJobs;
        int64_t meshBytes = 0;
        int64_t triangles = 0;

        // the coverage grid of an attached Open volume (R41)
        std::shared_ptr<const voxel::Coverage> coverage;

        // collision (R21-R24)
        std::map<std::tuple<int, int, int>, VoxelCollisionBlock> collision;
        std::deque<std::tuple<int, int, int>> readyBodies;
        std::deque<VoxelCollisionSwap> swaps;
        uint32_t collisionEdits = 0;
        bool collisionStarted = false;
        int bodies = 0;
#ifdef JASPERENGINE_PHYSICS_3D
        struct PendingAdd{
            // Shared and never moved: a prepare job reads them, while the list of adds may reallocate
            std::shared_ptr<std::vector<JPH::BodyID>> ids;
            std::vector<std::tuple<int, int, int>> keys;
            JPH::BodyInterface::AddState state = nullptr;
            jobs::Task<JPH::BodyInterface::AddState> task;
            bool hasTask = false;
        };
        std::vector<PendingAdd> pendingAdds;
#endif

        // rescue (R24)
        struct BodyWatch{
            int deepChecks = 0;
            bool rescuedOnce = false;
            bool hasFree = false;
            Vector3 lastFree;
            bool fast = false;
        };
        std::unordered_map<Entity, BodyWatch> watches;
        size_t rescueCursor = 0;
        int rescues = 0;
        int shapesBuiltOnMain = 0;

        // events (R31), and the bounds waiting for navigation (R25)
        std::vector<VoxelEditEvent> events;
        std::vector<AABB> collisionEvents;
        std::vector<Entity> rescuedEvents;
        bool navDirty = false;
        AABB navBounds;
        double navSinceFirst = 0.0;
        double navSinceLast = 0.0;

        // terrain link (R38-R46)
        Entity attachedTerrain = NULL_ENTITY;
        bool holdingTerrainMap = false;
        std::vector<uint8_t> cutMask;
        int cutWidth = 0, cutHeight = 0;
        // R44: runs (row, start, length) waiting for the render activation of their edit
        std::vector<std::array<int, 3>> pendingOpens;
        uint64_t pendingOpenSet = 0;
        std::vector<uint8_t> columnFlush;
        terrain::MapId holeLattice;
        int coverageRing = 0;
        int lastHolesCut = 0;
        int lastHolesFilled = 0;
        bool lastEntranceNarrow = false;
        // R46: the terrain's dirty rectangles reach us through T1's events
        Subscription terrainEdits;
        std::vector<terrain::TexelRect> terrainDirty;

        double lastMeshMs = 0.0;
        uint64_t latencyStartFrame = 0;
        int lastLatencyFrames = -1;
        int64_t uploadBytesThisFrame = 0;
        int uploadMeshesThisFrame = 0;
    };

}

#endif
