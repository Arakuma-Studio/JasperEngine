// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.1)
// - Purpose: shared, immutable Vertex Animation assets keyed by normalized .vat path (the
//   AnimationClipPool pattern): reads the .vat JSON and its .vat.glb through Data::open (resource
//   packs work), checks the glb's layoutHash against the JSON, builds vat::VertexAnimation.
// - Thread-safety: mutex-guarded; getOrLoad may run on any thread (the GPU texture is created later,
//   on first VertexAnimation::getTexture, on the main thread).
// - Engine::clearPools / clearUnusedPools call clear / clearUnused; the editor calls invalidate
//   after a (re)bake or an external edit, which bumps getGeneration(path).

#ifndef VERTEX_ANIMATION_POOL_H
#define VERTEX_ANIMATION_POOL_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct ClipWarning;
    namespace vat{ class VertexAnimation; }

    class JASPERENGINE_API VertexAnimationPool{
    public:
        static std::string getKey(const std::string& path);

        // cached asset or null (no load)
        static std::shared_ptr<const vat::VertexAnimation> get(const std::string& path);
        // cached or loaded now; null with `error` set when the files can't be read, parsed or don't match
        static std::shared_ptr<const vat::VertexAnimation> getOrLoad(const std::string& path, std::string* error = nullptr,
                                                                     std::vector<ClipWarning>* warnings = nullptr);

        // registers an in-memory asset under `path` (baker preview, tests): replaces any cached one,
        // bumps the generation; the asset's getPath() becomes the key
        static void put(const std::string& path, std::shared_ptr<const vat::VertexAnimation> asset);

        static uint64_t getGeneration(const std::string& path);
        static void invalidate(const std::string& path);

        static void clear();
        static void clearUnused();
        static size_t size();
    };

}

#endif //VERTEX_ANIMATION_POOL_H
