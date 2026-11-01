// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 7.2)
// - Purpose: the shared cache of Avatar Masks (.mask): one shared_ptr<const anim::AvatarMask> per
//   normalized path, read through Data::open (resource packs work). AnimationClipPool's .anim
//   rules: asset-relative or absolute paths, generations that only grow (a player that bound a
//   mask's generation re-resolves its track weights when it changes), failures not cached.
// - Engine::clearPools / clearUnusedPools clear it with the other pools.
// - Thread-safety: one mutex (JsonAssetPool); a mask a caller holds stays valid after
//   invalidate/clear.

#ifndef AVATAR_MASK_POOL_H
#define AVATAR_MASK_POOL_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace anim{
        class AvatarMask;
    }

    class JASPERENGINE_API AvatarMaskPool{
    public:
        static std::string getKey(const std::string& path);
        // the cached mask, null when not loaded (never loads)
        static std::shared_ptr<const anim::AvatarMask> get(const std::string& path);
        // the cached mask, else loads and caches it; null + `error` (the parse error with
        // line:col, a missing file, the Unity YAML refusal) on failure
        static std::shared_ptr<const anim::AvatarMask> getOrLoad(const std::string& path, std::string* error = nullptr);
        // 0 when never loaded
        static uint64_t getGeneration(const std::string& path);
        static uint64_t revision();
        // the editor changed the file: dropped, the next getOrLoad reads it again
        static void invalidate(const std::string& path);
        static void clear();
        static void clearUnused();
        static size_t size();
    };

}

#endif // AVATAR_MASK_POOL_H
