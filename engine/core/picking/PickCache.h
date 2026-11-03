// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The coalescing (one build per geometry, shared by every entity drawing it), the pending
// policy and the reference-counted drop are adapted from OpenEngine's editor MeshBvhCache
// (Apps/Editor/Source/Picking/MeshBvhCache.cpp), Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License (NOTICE.md). Here it is the engine's (the runtime MeshRaycast
// uses it too), builds run as S06 Background jobs, and the disk cache is a hook the editor sets.

#ifndef PICKCACHE_H
#define PICKCACHE_H

#include "Export.h"
#include "jobs/Task.h"
#include "picking/MeshBvh.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace jasperengine {

    // Where built trees are kept between sessions (the editor's .jasperengine/cache/meshbvh/):
    // the cache asks it before building a large mesh and gives it what it built. Called from
    // build jobs (any thread).
    class JASPERENGINE_API PickDiskCache {
    public:
        virtual ~PickDiskCache() = default;
        // true with `bvh` filled from a valid file of this hash; anything else rebuilds
        virtual bool load(uint64_t hash, const PickGeometry& geometry, MeshBvh& bvh) = 0;
        virtual void store(uint64_t hash, const MeshBvh& bvh) = 0;
    };

    struct PickCacheStats {
        size_t built = 0;           // trees ready
        size_t pending = 0;         // queued or building
        size_t bytes = 0;           // what the ready trees keep
        size_t entries = 0;         // built, pending and dropped-but-kept ones
        double buildMsTotal = 0.0;  // build (or disk load) time of every tree so far
        uint64_t builds = 0;        // trees built since the start
        uint64_t diskLoads = 0;     // trees loaded from the disk cache
        uint64_t inlineWaits = 0;   // queries that had to wait for (or build) a tree
    };

    class JASPERENGINE_API PickCache {
    public:
        // meshes of at least this many triangles go to the disk cache (R10)
        static constexpr uint32_t DISK_CACHE_TRIANGLES = 65536u;
        // an entry nobody uses is dropped this long after its last user went
        static constexpr double DROP_AFTER_SECONDS = 30.0;

        struct Entry {
            uint64_t hash = 0;
            uint32_t triangles = 0;
            // set once (before ready), never changed after
            std::shared_ptr<const MeshBvh> bvh;
            std::atomic<bool> ready{false};
            // users and release time are the cache's (its mutex)
            uint32_t users = 0;
            std::chrono::steady_clock::time_point released;
            // the build's input until it ran, and its job
            std::shared_ptr<PickGeometry> geometry;
            jobs::TaskRef task;
            std::mutex buildMutex;
            double buildMs = 0.0;
            size_t bytes = 0;
            bool fromDisk = false;
        };
        using EntryPtr = std::shared_ptr<Entry>;

        static PickCache& get();

        PickCache() = default;
        ~PickCache();
        PickCache(const PickCache&) = delete;
        PickCache& operator=(const PickCache&) = delete;

        // The entry of a geometry hash with one more user, or null when there is none
        EntryPtr acquire(uint64_t hash);
        // A new entry (one user) building `geometry`: as a Background job, or right here
        // when `inlineBuild` (or without a job system). An entry of that hash made meanwhile
        // is returned instead.
        EntryPtr create(uint64_t hash, PickGeometry&& geometry, bool inlineBuild = false);
        void release(const EntryPtr& entry);

        // The tree when built, else null
        static const MeshBvh* ready(const EntryPtr& entry);
        // Waits for the tree (running its job here when nobody started it; building it here
        // when the job was cancelled) and returns it
        const MeshBvh* wait(const EntryPtr& entry);

        // Drops the unused entries released more than DROP_AFTER_SECONDS before `now`
        void collect(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        // Every entry goes (their users keep theirs alive until they release them)
        void clear();

        PickCacheStats stats() const;

        // The editor's disk cache (not owned; null: none, as in exported games)
        void setDiskCache(PickDiskCache* disk);
        PickDiskCache* getDiskCache() const;

        // Meshes above this many triangles pick by their bounds (Engine::setMeshRaycastMaxTriangles)
        static void setMaxTriangles(uint32_t triangles);
        static uint32_t getMaxTriangles();

    private:
        mutable std::mutex mutex;
        std::unordered_map<uint64_t, EntryPtr> entries;
        std::atomic<PickDiskCache*> disk{nullptr};
        std::chrono::steady_clock::time_point lastCollect;

        std::atomic<uint64_t> builds{0};
        std::atomic<uint64_t> diskLoads{0};
        std::atomic<uint64_t> inlineWaits{0};
        double buildMsTotal = 0.0; // the mutex's

        static std::atomic<uint32_t> maxTriangles;

        void runBuild(const EntryPtr& entry);
        void collectLocked(std::chrono::steady_clock::time_point now);
    };

}

#endif // PICKCACHE_H
