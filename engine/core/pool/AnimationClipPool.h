// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.1, 9.R11/R13)
// - Purpose: the shared cache of runtime clips: one shared_ptr<const AnimationClip> per
//   normalized key, for .anim files (read through Data::open, so resource packs work) and model
//   sub-clips ("<model>#<name|index>", built by ModelSubClips and keyed "<model key>#<index>").
// - Owner: T0 (API); T1a implements it (+ Engine::clearPools / clearUnusedPools hooks and the
//   ModelPool::invalidate lock), T1c the sub-clip branch of getOrLoad.
// - Thread-safety: every function takes the pool mutex; loads of different keys may run
//   concurrently (play startup and the shader-variant probe run off the main thread). A clip a
//   caller holds stays valid after invalidate/clear (shared ownership).
// - Invariants:
//   * a key's generation increases on every invalidate or reload of it and never decreases, so
//     a player that remembers the generation it bound to re-fetches (keeping its rest) when it
//     changes; revision() increases on any change of any key (a cheap per-frame "anything?").
//   * invalidateModel(model) drops every "<model key>#*" entry.
//   * failures are not cached: getOrLoad tries again on the next call (callers throttle, the
//     players every 1 s, PoseAnimator style).

#ifndef ANIMATION_CLIP_POOL_H
#define ANIMATION_CLIP_POOL_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class AnimationClip;
    struct ClipWarning;

    class JASPERENGINE_API AnimationClipPool{
    public:
        // The cache key of a clip reference (asset-relative or absolute .anim path, or a
        // sub-clip reference): what get/invalidate compare
        static std::string getKey(const std::string& path);

        // the cached clip, null when not loaded (never loads)
        static std::shared_ptr<const AnimationClip> get(const std::string& path);
        // the cached clip, else loads and caches it; null + `error` (the parse error with
        // line:col, a missing file, an unknown sub-clip) on failure. `warnings` (optional): the
        // load's warnings, also kept on the clip
        static std::shared_ptr<const AnimationClip> getOrLoad(const std::string& path, std::string* error = nullptr,
                                                              std::vector<ClipWarning>* warnings = nullptr);

        // 0 when never loaded
        static uint64_t getGeneration(const std::string& path);
        static uint64_t revision();

        // The editor changed the file (document commit, undo, auto-sync): dropped even while
        // referenced, so the next getOrLoad reads it again
        static void invalidate(const std::string& path);
        // A model file changed: every sub-clip of it
        static void invalidateModel(const std::string& modelPath);

        // engine shutdown / project change
        static void clear();
        // drops the entries no one else references
        static void clearUnused();

        static size_t size();
    };

}

#endif //ANIMATION_CLIP_POOL_H
