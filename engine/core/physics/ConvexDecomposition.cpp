// SPDX-License-Identifier: MIT
// STUB (contract only): replaced by the V-HACD 4 implementation.

#include "ConvexDecomposition.h"

using namespace jasperengine;

DecompositionParams DecompositionParams::fromQuality(DecompositionQuality quality) {
    DecompositionParams p;
    (void)quality;
    return p;
}

bool DecompositionParams::operator==(const DecompositionParams& o) const {
    return maxHulls == o.maxHulls && resolution == o.resolution && maxVerticesPerHull == o.maxVerticesPerHull;
}

uint64_t jasperengine::hashDecompositionInput(const std::vector<Vector3>&, const std::vector<uint32_t>&, const DecompositionParams&) {
    return 0;
}

struct ConvexDecompositionCache::Impl {
    std::string directory;
};

ConvexDecompositionCache::ConvexDecompositionCache(std::string directory): impl(new Impl{std::move(directory)}) {}
ConvexDecompositionCache::~ConvexDecompositionCache() = default;
const std::string& ConvexDecompositionCache::getDirectory() const { return impl->directory; }
bool ConvexDecompositionCache::find(uint64_t, std::vector<ConvexHullPoints>&) const { return false; }
void ConvexDecompositionCache::store(uint64_t, const std::vector<ConvexHullPoints>&) {}
DecompositionStatus ConvexDecompositionCache::status(uint64_t) const { return DecompositionStatus::NONE; }
uint64_t ConvexDecompositionCache::request(const std::vector<Vector3>& p, const std::vector<uint32_t>& i, const DecompositionParams& params) {
    return hashDecompositionInput(p, i, params);
}
void ConvexDecompositionCache::cancel(uint64_t) {}
void ConvexDecompositionCache::cancelAll() {}
size_t ConvexDecompositionCache::pendingCount() const { return 0; }
uint64_t ConvexDecompositionCache::completionVersion() const { return 0; }
void ConvexDecompositionCache::clear() {}
size_t ConvexDecompositionCache::entryCount() const { return 0; }
size_t ConvexDecompositionCache::bytesOnDisk() const { return 0; }
bool ConvexDecompositionCache::decompose(const std::vector<Vector3>&, const std::vector<uint32_t>&, const DecompositionParams&,
                                         std::vector<ConvexHullPoints>& out, const std::atomic<bool>*, std::string* error) {
    out.clear();
    if (error) *error = "convex decomposition is not implemented yet";
    return false;
}
