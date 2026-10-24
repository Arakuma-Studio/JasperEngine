// SPDX-License-Identifier: MIT
//
// Convex decomposition for edit-time physics placement (Physics Placement, design spec
// docs/superpowers/specs/2026-10-23-physics-placement-design.md section 5.3).
//
// Splits a concave mesh into a few convex hulls with V-HACD 4 (engine/libs/vhacd, BSD-3),
// on a background thread, and caches the result by geometry content so a re-imported or edited
// mesh never serves stale hulls. Hull points are in the mesh's own space, unscaled: the caller
// applies the entity's world scale.
//
// No Jolt types here; PlacementWorld turns the hulls into shapes.

#pragma once

#include "Export.h"
#include "math/Vector3.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    enum class DecompositionQuality {
        LOW,
        MEDIUM,
        HIGH
    };

    struct JASPERENGINE_API DecompositionParams {
        uint32_t maxHulls = 8;
        uint32_t resolution = 400000;      // voxels
        uint32_t maxVerticesPerHull = 64;

        static DecompositionParams fromQuality(DecompositionQuality quality);
        bool operator==(const DecompositionParams& other) const;
    };

    struct ConvexHullPoints {
        std::vector<Vector3> points;
    };

    // The cache key: geometry (positions welded by exact value, triangle indices), the
    // parameters and the algorithm version. Same geometry and params: same key on every platform.
    // Welding maps every copy of a position (UV seams, split normals) to its first index; -0 and
    // +0 are the same value. Triangles that collapse after welding, triangles with an index out
    // of range and a trailing partial triangle are left out, as decompose() leaves them out or
    // refuses them.
    JASPERENGINE_API uint64_t hashDecompositionInput(const std::vector<Vector3>& positions,
                                                     const std::vector<uint32_t>& indices,
                                                     const DecompositionParams& params);

    enum class DecompositionStatus {
        NONE,       // never requested and not cached
        PENDING,    // queued or running
        READY,      // cached (memory or disk)
        FAILED      // the last attempt failed (degenerate mesh, cancelled, ...); not retried until
                    // cleared, except a cancelled one: request() queues it again
    };

    // Thread-safe: every method may be called from any thread. One instance per project (editor)
    // or per run (CLI). Jobs run one at a time on a worker thread the cache owns, started by the
    // first request() and joined by cancelAll() and the destructor.
    //
    // Disk entries are <directory>/<key as 16 hex digits>.hulls: little-endian, a magic number and
    // a format version, the key, the hull count, then per hull its point count and float3 points.
    // Written aside (a name unique to the process and write) and renamed into place, tried twice,
    // then reported by lastDiskError() (the hulls stay in memory); a file that does not read back
    // whole is a miss and is deleted. Several caches and processes (the editor, a CLI run) may
    // share a directory.
    //
    // Builds without JASPERENGINE_CONVEX_DECOMPOSITION (exported games, Web, Android, iOS) keep
    // find(), store() and clear(); request() marks the key FAILED and decompose() fails, both with
    // "convex decomposition is not available in this build".
    class JASPERENGINE_API ConvexDecompositionCache {
    public:
        // directory: where <key hex>.hulls files live (created on first store); "" keeps the cache
        // in memory only.
        explicit ConvexDecompositionCache(std::string directory);
        ~ConvexDecompositionCache();

        ConvexDecompositionCache(const ConvexDecompositionCache&) = delete;
        ConvexDecompositionCache& operator=(const ConvexDecompositionCache&) = delete;

        const std::string& getDirectory() const;

        // Memory first, then disk. False when not cached. A key missed on disk is not looked for
        // there again until request(), store() or clear() (so polling status() stays cheap): an
        // entry another process writes meanwhile is found by the next request().
        bool find(uint64_t key, std::vector<ConvexHullPoints>& out) const;
        void store(uint64_t key, const std::vector<ConvexHullPoints>& hulls);

        DecompositionStatus status(uint64_t key) const;

        // Queues a background decomposition unless the key is cached, pending or failed (a key
        // that failed by being cancelled is queued again). A key on disk is loaded instead.
        // Returns the key. Inputs are copied. Not urgent: behind every job already queued.
        uint64_t request(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                         const DecompositionParams& params);
        // urgent: ahead of every job that is not urgent, behind the urgent ones queued before it
        // (urgent jobs run first come, first served), for a caller waiting on the result (an
        // agent run with decomposition "always") while a bake has queued many. A key already
        // queued without urgency moves up the same way. The running job always finishes first:
        // nothing is preempted. urgent false is the overload above.
        uint64_t request(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                         const DecompositionParams& params, bool urgent);

        // Makes a queued key urgent, as request(..., true) would: ahead of every job that is not
        // urgent (the front of the queue when no other job is urgent), behind earlier urgent ones.
        // No effect on a key that is running, already urgent, done or unknown.
        void prioritize(uint64_t key);

        // A queued job is dropped, a running one stops as soon as V-HACD notices; either way the
        // key becomes FAILED with "cancelled" (and completionVersion bumps), and a later
        // request() retries it. No effect on a key that is not pending.
        void cancel(uint64_t key);
        void cancelAll();          // also waits for running jobs to stop
        size_t pendingCount() const;   // queued plus running

        // Bumps whenever a job finishes (ready, failed or cancelled), so callers can poll cheaply.
        uint64_t completionVersion() const;

        // Why the key is FAILED ("cancelled", "mesh has no volume", ...); "" when it is not.
        std::string failureReason(uint64_t key) const;

        // Why the last entry could not be saved to disk, after two tries ("could not rename
        // ..."); "" once a save succeeds, after clear(), and for a memory-only cache.
        std::string lastDiskError() const;

        // Removes memory and disk entries and forgets failures. Jobs still pending are not
        // cancelled: they store their result when they finish. Temporary files of other writers
        // are left alone unless they are over an hour old.
        void clear();
        size_t entryCount() const;   // memory + disk entries, each key once
        size_t bytesOnDisk() const;

        // Synchronous decomposition (the job body). cancel may be null. False and *error on
        // failure; a mesh with no volume fails, and so do fewer than 4 distinct vertices, no
        // triangle, an index out of range, a non-finite position, parameters out of range
        // (maxHulls 0, resolution outside 10000..64000000, maxVerticesPerHull under 4) and a
        // V-HACD error. The input is welded as in hashDecompositionInput(). Blocks the calling
        // thread while V-HACD's job pool (one thread less than the machine has, 1 to 8) splits
        // and merges hulls; the result does not depend on the thread count.
        static bool decompose(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                              const DecompositionParams& params, std::vector<ConvexHullPoints>& out,
                              const std::atomic<bool>* cancel, std::string* error);

        // False when the build has no V-HACD (JASPERENGINE_CONVEX_DECOMPOSITION off).
        static bool available();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
