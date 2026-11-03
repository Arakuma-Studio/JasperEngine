// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D13, R35)
// - Purpose: the shared cache of the Animator's assets: compiled controllers (.controller),
//   avatar masks (.mask) and montages (.montage), one immutable value per normalized path
//   (AnimationClipPool's key rule: FileData::getSystemPath, lexically normal, generic
//   separators), read through Data::open so resource packs work.
// - Generations: one pool-wide counter; a key's generation increases on every load after an
//   invalidate and never decreases, revision() on any change (Animators compare it once a
//   frame and rebind what changed, keeping their states by name).
// - Failures are not cached: the next call reads the file again (callers throttle, 1 s).
// - Thread-safety: every function takes the pool mutex, never held while a file loads.

#ifndef ANIMATOR_ASSET_POOL_H
#define ANIMATOR_ASSET_POOL_H

#include "Export.h"
#include "animation/ClipTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace anim{
        struct CompiledController;
        struct AvatarMaskDef;
        struct MontageDef;
    }

    class JASPERENGINE_API AnimatorAssetPool{
    public:
        static std::string getKey(const std::string& path);

        // the cached asset, else loads it; null + `error` (with line:col for parse errors) on
        // failure. `generation` (optional) gets the key's generation, `warnings` the load's.
        static std::shared_ptr<const anim::CompiledController> controller(const std::string& path, std::string* error = nullptr,
                                                                         uint64_t* generation = nullptr,
                                                                         std::vector<ClipWarning>* warnings = nullptr);
        static std::shared_ptr<const anim::AvatarMaskDef> mask(const std::string& path, std::string* error = nullptr,
                                                              uint64_t* generation = nullptr, std::vector<ClipWarning>* warnings = nullptr);
        static std::shared_ptr<const anim::MontageDef> montage(const std::string& path, std::string* error = nullptr,
                                                              uint64_t* generation = nullptr, std::vector<ClipWarning>* warnings = nullptr);

        // 0 when never loaded
        static uint64_t getGeneration(const std::string& path);
        static uint64_t revision();

        // the file changed (editor commit, undo, auto-sync, hot reload): dropped, the next call reads it
        static void invalidate(const std::string& path);
        static void clear();
        // drops the entries nobody else references
        static void clearUnused();
        static size_t size();
    };

}

#endif //ANIMATOR_ASSET_POOL_H
