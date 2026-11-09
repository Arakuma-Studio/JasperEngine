// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_CHANGE_SET_H
#define VOXEL_CHANGE_SET_H

// R17, V6: never a crack. Whatever goes on screen is part of a change set: a list of (block, level,
// side mask) at one data version, built off screen. The old meshes stay until every mesh of the set
// is built and uploaded, then one activation swaps them, so a failed or a cancelled job can only
// delay a set and never shows anything wrong. Pure logic: the scheduling is a test's to shuffle.

#include "Export.h"
#include "TransvoxelMesher.h"
#include "VoxelClipbox.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace jasperengine::voxel {

    // A mesh's identity: the block, its mask, and the version of the data its window read
    struct MeshId {
        BlockKey key;
        uint8_t sideMask = 0;
        uint64_t windowVersion = 0;
        bool operator==(const MeshId& other) const { return key == other.key && sideMask == other.sideMask && windowVersion == other.windowVersion; }
        bool operator<(const MeshId& other) const {
            if (key != other.key) return key < other.key;
            if (sideMask != other.sideMask) return sideMask < other.sideMask;
            return windowVersion < other.windowVersion;
        }
    };

    struct MeshBuildRequest {
        MeshId id;
        bool editPriority = false;
    };

    struct ChangeSetOptions {
        // R17's padding: samples of a block's level around it whose change means a remesh. Regular
        // cells read samples -1..17, a masked side's coarse window -2..18. Only the red control of
        // AC16 sets 0.
        int dirtyPadding = 2;
        // R17's "small cache" of finished meshes no set needs at the moment
        size_t cacheCapacity = 256;
    };

    class JASPERENGINE_API ChangeSetScheduler {
    public:
        explicit ChangeSetScheduler(const ChangeSetOptions& options = {});

        // What the clipbox wants. Starts an LOD set unless an edit set is in flight or one LOD set
        // already is: then it waits, replaced by anything newer.
        void setDesiredLod(const std::vector<VisibleBlock>& desired);

        // An edit landed: changed samples in voxels. Cancels a pending LOD set, and every visible (or
        // in-flight) block whose window read those samples is rebuilt, in one set. A third set merges into the
        // second. levels is the store's level count.
        // Returns the set this edit lands in: R23 swaps collision at or after its activation.
        uint64_t onEdit(const IntBox& changed, int levels);
        // The newest set that went on screen; sets activate in order
        uint64_t lastActivatedSet() const { return lastActivated; }

        // The builds to start now. The caller may run them in any order.
        void takeRequests(std::vector<MeshBuildRequest>& out);
        // Requests no longer wanted, because an LOD set was cancelled: a queue may drop them. A
        // running job's result is still welcome in the cache.
        void takeCancelled(std::vector<MeshId>& out);

        // A build finished. The mesh goes in the cache even if no set wants it any more.
        void meshBuilt(const MeshId& id, std::shared_ptr<const MeshOut> mesh);
        // A build failed or was dropped: it is requested again, the set simply waits.
        void meshFailed(const MeshId& id);

        // What needs uploading for the set in flight: built and not yet uploaded, with triangles
        void takeUploads(std::vector<std::pair<MeshId, std::shared_ptr<const MeshOut>>>& out);
        void meshUploaded(const MeshId& id);

        struct Activation {
            std::vector<MeshId> hide;
            std::vector<std::pair<MeshId, std::shared_ptr<const MeshOut>>> show;
            bool wasEditSet = false;
            uint64_t setId = 0;
            IntBox changedBounds;     // an edit set's changes, voxels
        };
        // The next set that is complete, in order. One per call.
        bool pollActivation(Activation& out);

        // The caller's chunks for this mesh were dropped when hidden, so a reuse must upload again
        void meshReleased(const MeshId& id);

        // After an edit set activated, the LOD set is computed afresh (R17)
        bool wantsLodRefresh() const { return lodRefresh; }
        void clearLodRefresh() { lodRefresh = false; }

        bool anyActivated() const { return activations > 0; }
        int inFlight() const { return static_cast<int>(sets.size()); }
        bool idle() const { return sets.empty() && !waitingLod; }
        const std::map<BlockKey, MeshId>& visible() const { return current; }
        uint64_t windowVersionOf(const BlockKey& key) const;
        // Meshes held, for stats
        size_t cachedMeshes() const { return meshes.size(); }

    private:
        struct MeshEntry {
            std::shared_ptr<const MeshOut> mesh;
            bool built = false;
            bool requested = false;
            bool uploaded = false;
            bool uploadHandedOut = false;
            uint64_t lastUse = 0;
        };

        struct ChangeSet {
            uint64_t id = 0;
            bool isEdit = false;
            std::map<BlockKey, MeshId> target;
            IntBox changed;
        };

        void startSet(bool isEdit, std::map<BlockKey, MeshId> target, const IntBox& changed);
        void request(const MeshId& id, bool editPriority);
        bool setComplete(const ChangeSet& set) const;
        void markDirty(const IntBox& changed, int levels);
        MeshId idFor(const BlockKey& key, uint8_t sideMask) const;
        void trimCache();
        void startWaitingLod();
        const std::map<BlockKey, MeshId>& latestTarget() const;

        ChangeSetOptions options;
        std::map<MeshId, MeshEntry> meshes;
        std::map<BlockKey, uint64_t> windowVersions;
        std::map<BlockKey, MeshId> current;
        std::vector<ChangeSet> sets;
        std::vector<MeshBuildRequest> pendingRequests;
        std::vector<MeshId> cancelled;
        std::vector<VisibleBlock> waitingDesired;
        bool waitingLod = false;
        bool lodRefresh = false;
        uint64_t nextSetId = 1;
        uint64_t editVersion = 0;
        uint64_t clock = 0;
        int activations = 0;
        uint64_t lastActivated = 0;
    };

}

#endif
