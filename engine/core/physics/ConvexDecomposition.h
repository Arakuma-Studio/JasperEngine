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
    JASPERENGINE_API uint64_t hashDecompositionInput(const std::vector<Vector3>& positions,
                                                     const std::vector<uint32_t>& indices,
                                                     const DecompositionParams& params);

    enum class DecompositionStatus {
        NONE,       // never requested and not cached
        PENDING,    // queued or running
        READY,      // cached (memory or disk)
        FAILED      // the last attempt failed (degenerate mesh, cancelled, ...); not retried until cleared
    };

    // Thread-safe. One instance per project (editor) or per run (CLI).
    class JASPERENGINE_API ConvexDecompositionCache {
    public:
        // directory: where <key hex>.hulls files live (created on first store); "" keeps the cache
        // in memory only.
        explicit ConvexDecompositionCache(std::string directory);
        ~ConvexDecompositionCache();

        ConvexDecompositionCache(const ConvexDecompositionCache&) = delete;
        ConvexDecompositionCache& operator=(const ConvexDecompositionCache&) = delete;

        const std::string& getDirectory() const;

        // Memory first, then disk. False when not cached.
        bool find(uint64_t key, std::vector<ConvexHullPoints>& out) const;
        void store(uint64_t key, const std::vector<ConvexHullPoints>& hulls);

        DecompositionStatus status(uint64_t key) const;

        // Queues a background decomposition unless the key is cached, pending or failed.
        // Returns the key. Inputs are copied.
        uint64_t request(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                         const DecompositionParams& params);

        void cancel(uint64_t key);
        void cancelAll();          // also waits for running jobs to stop
        size_t pendingCount() const;

        // Bumps whenever a job finishes (ready or failed), so callers can poll cheaply.
        uint64_t completionVersion() const;

        // Removes memory and disk entries and forgets failures.
        void clear();
        size_t entryCount() const;   // memory + disk entries
        size_t bytesOnDisk() const;

        // Synchronous decomposition (the job body). cancel may be null. False and *error on
        // failure; a mesh with no volume fails.
        static bool decompose(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices,
                              const DecompositionParams& params, std::vector<ConvexHullPoints>& out,
                              const std::atomic<bool>* cancel, std::string* error);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
