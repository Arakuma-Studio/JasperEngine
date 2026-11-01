// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 11.1, 11.6)
// - Purpose: the shared cache of Animator Controllers (.controller): one
//   shared_ptr<const anim::AnimatorController> per normalized path, read through Data::open
//   (resource packs work: exported builds load them like clips). AnimationClipPool's .anim
//   rules: asset-relative or absolute paths, generations that only grow (a player that loaded a
//   controller's generation rebuilds its states when it changes), failures not cached.
// - Engine::clearPools / clearUnusedPools clear it with the other pools.
// - Thread-safety: one mutex (JsonAssetPool); a controller a caller holds stays valid after
//   invalidate/clear.

#ifndef ANIMATOR_CONTROLLER_POOL_H
#define ANIMATOR_CONTROLLER_POOL_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace anim{
        class AnimatorController;
    }

    class JASPERENGINE_API AnimatorControllerPool{
    public:
        static std::string getKey(const std::string& path);
        // the cached controller, null when not loaded (never loads)
        static std::shared_ptr<const anim::AnimatorController> get(const std::string& path);
        // the cached controller, else loads and caches it; null + `error` (the parse error with
        // line:col, a missing file, the Unity YAML refusal) on failure
        static std::shared_ptr<const anim::AnimatorController> getOrLoad(const std::string& path, std::string* error = nullptr);
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

#endif // ANIMATOR_CONTROLLER_POOL_H
