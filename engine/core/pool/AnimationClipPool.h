// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.1, 9.R11/R13)
// - Purpose: the shared cache of runtime clips: one shared_ptr<const AnimationClip> per
//   normalized key, for .anim files (read through Data::open, so resource packs work) and model
//   sub-clips ("<model>#<name|index>", built by ModelSubClips and keyed "<model key>#<index>").
// - Engine::clearPools / clearUnusedPools clear it with the other pools.
// - Thread-safety: every function takes the pool mutex; loads of different keys may run
//   concurrently (play startup and the shader-variant probe run off the main thread). A clip a
//   caller holds stays valid after invalidate/clear (shared ownership). Details:
//   * concurrent getOrLoad of the same key: one load is cached and every caller gets that clip;
//     the generation is bumped once;
//   * invalidate during an in-flight load: the load's result is returned to its caller but not
//     cached (the next getOrLoad reads the file again);
//   * lock order: the pool mutex is never held while loading or building (a sub-clip build takes
//     the pooled-glTF mutex): look up, unlock, load, lock, insert.
// - Keys (one form for the editor document, the players, MCP and the CLI):
//   * a .anim path, asset-relative (the component form, "anim/walk.anim") or absolute, normalizes
//     to one key: FileData::getSystemPath, lexically normal, generic separators. The engine has no
//     project root, so project-relative "assets/..." paths are NOT accepted: editor callers pass
//     AnimationClipDocument::absolutePath() (or the component's asset-relative form).
//   * a sub-clip reference "<model>#<clip>" normalizes its model part the same way and keeps the
//     clip part as given: "<model key>#<name or index>". The entry itself is stored under
//     "<model key>#<animation index>" (R11: never the name). Per model the pool keeps an alias
//     table (every sub-clip name, and every index as a number, -> animation index) filled the
//     first time any sub-clip of that model is loaded (anim::listModelFileClips), so
//     "hero.gltf#Walk" and "hero.gltf#0" share one entry and one generation. get()/getGeneration()
//     of an alias the table does not know yet (model never loaded) return null / 0; getOrLoad
//     fills the table first. invalidateModel drops the entries and the alias table.
// - Invariants:
//   * generations come from one pool-wide counter (round 1 of the T1a review): no two versions
//     of any keys share a number, so a sub-clip alias that resolves to another index entry after
//     a reimport also reads as changed. A sub-clip load is cached only when neither its entry
//     nor the model's alias table it resolved through was invalidated meanwhile.
//   * a key's generation increases on every invalidate or reload of it and never decreases, so
//     a player that remembers the generation it bound to re-fetches (keeping its rest) when it
//     changes; revision() increases on any change of any key (a cheap per-frame "anything?").
//   * failures are not cached: getOrLoad tries again on the next call (callers throttle, the
//     players every 1 s, PoseAnimator style).
// - Tested: invalidate through AnimationClipDocument::absolutePath() reaches a clip a player
//   loaded through the asset-relative component path.
// - Sub-clip branch: the alias table comes from
//   anim::listModelFileClips(model), the clip from anim::buildModelSubClip("<model>#<name>")
//   with the name of the resolved index. The builders read the pooled tinygltf model under the
//   pooled-glTF mutex (MeshSystem::getPooledGLTFMutex), never under the pool mutex.
//   MeshSystem::invalidateModelFile and invalidateModelParse call invalidateModel (the editor's
//   model reload, R13). For a changed model file editors call one of those, not
//   invalidate/invalidateModel alone: the builders prefer the pooled parse, which only they drop
//   (invalidateModelParse when the entities using the model are not reloaded: their images stay).
// - Humanoid clips (anim-retarget 7.4): a sub-clip of a model whose .humanoid sidecar says
//   animation_type humanoid is converted (anim::convertToHumanoid, the model's rig from
//   anim::retargetSource) after it is built; its key is unchanged. invalidateModel (a model or
//   sidecar edit: the editor's sidecar writers call it) drops it with the model's other clips.
//   A Retarget entry or an Animator retargeting a generic clip loads the clip as it is: the one
//   retarget path (animation/AnimatorRetarget.h) reads it through its source model at play time.

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
        // The normalized form of a clip reference (see Keys above); sub-clip references keep
        // their clip part as given (the alias table resolves it)
        static std::string getKey(const std::string& path);

        // the cached clip, null when not loaded (never loads)
        static std::shared_ptr<const AnimationClip> get(const std::string& path);
        // the cached clip, else loads and caches it; null + `error` (the parse error with
        // line:col, a missing file, an unknown sub-clip) on failure. `warnings` (optional): the
        // load's warnings, also kept on the clip
        static std::shared_ptr<const AnimationClip> getOrLoad(const std::string& path, std::string* error = nullptr,
                                                              std::vector<ClipWarning>* warnings = nullptr);

        // getOrLoad that never waits for a build (final review M1, S74 R16): in async loading mode
        // (Engine::isAsyncLoading) a clip not cached yet builds on the thread pool, since a model
        // sub-clip's builder may wait for the pooled-glTF mutex a model loading on another thread
        // holds for its whole build, or parse the file, and a .anim parses about 50 ms per MB: null
        // with `loading` true until the build is done, then what getOrLoad gave (the clip, or null
        // + `error`), to the first caller that polls it. Every load without async loading, a .anim
        // under 64 KB (a few milliseconds, where a job would start it a frame late) and a .anim
        // without threads (web without pthreads: a job would wait for the frame's pump) are
        // getOrLoad on this thread, with the same result. Players and scene preloads poll it once a
        // frame; invalidate, invalidateModel and clear forget the builds in flight (the next poll
        // starts another). `animInCall`: a caller that must show the pose in this call (an
        // editor preview, AnimationPlayer's sample) gets a .anim on this thread in any mode.
        static std::shared_ptr<const AnimationClip> pollOrLoad(const std::string& path, bool& loading, std::string* error = nullptr,
                                                               bool animInCall = false);

        // 0 when never loaded
        static uint64_t getGeneration(const std::string& path);
        static uint64_t revision();

        // The editor changed the file (document commit, undo, auto-sync): dropped even while
        // referenced, so the next getOrLoad reads it again
        static void invalidate(const std::string& path);
        // A model file changed: every sub-clip of it ("<model key>#*" and the alias table).
        // invalidate() of a model file path (.gltf/.glb/.fbx without '#') does the same (R13).
        static void invalidateModel(const std::string& modelPath);

        // engine shutdown / project change
        static void clear();
        // drops the entries no one else references
        static void clearUnused();
        // drops one entry when no one else references it (a scene preload's release)
        static void releaseUnused(const std::string& path);

        static size_t size();
        // the memoryBytes() of the clips cached now ("Animation.Clips" in memory::snapshot, the
        // Clip memory monitor)
        static uint64_t loadedBytes();

    };

}

#endif //ANIMATION_CLIP_POOL_H
