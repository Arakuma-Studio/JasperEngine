// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 9.R7)
// - Purpose: one edit-mode pose owner per model root. Everything that previews a pose on scene
//   entities in edit mode (the Animation window's clip and entity previews, the Inspector's
//   action preview, PoseEditSession) acquires the root first; acquiring stops the previous owner
//   of that root (or of a root above or below it) first, so every snapshot is taken from the
//   unpreviewed scene and restores happen in a safe order.
// - Owner: T0 (API); T3a implements it and makes the Animation window acquire it; the Pose
//   window (PoseEditSession::goIdle) and the Inspector action preview join in T3a too.
// - Thread-safety: main thread.
// - Invariants:
//   * at most one owner per (scene, overlapping root); a token is released once (release of a
//     stale token is a no-op);
//   * `stop` runs synchronously inside acquire() of another owner, never re-entrantly for the
//     same token: it must restore its snapshot and must not call acquire();
//   * releaseScene() forgets a closed scene's owners without calling their stop.

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor{

    class PreviewOwner{
    public:
        using Token = int;                          // 0 = none
        using Stop = std::function<void()>;

        struct Owner{
            Token token = 0;
            uint32_t sceneId = 0;
            Entity root = NULL_ENTITY;
            std::string name;                       // "Animation window", "Pose Animation", ... (logs, tests)
        };

        // Owns `root` of `sceneId` for an edit-mode preview: the owners of root, of an ancestor or
        // of a descendant (by `registry`; null: root only) are stopped first. Returns the token.
        static Token acquire(uint32_t sceneId, Entity root, EntityRegistry* registry, const std::string& name, Stop stop);
        static void release(Token token);
        static bool owns(Token token);
        // the owner of exactly that root, token 0 when none
        static Owner ownerOf(uint32_t sceneId, Entity root);
        static std::vector<Owner> owners();
        static void releaseScene(uint32_t sceneId);
    };

}
