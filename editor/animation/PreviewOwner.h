// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 9.R7)
// - Purpose: one edit-mode pose owner per model root. Everything that previews a pose on scene
//   entities in edit mode (the Animation window's clip and entity previews, the Inspector's
//   action preview, PoseEditSession) acquires the root first; acquiring stops the previous owner
//   of that root (or of a root above or below it) first, so every snapshot is taken from the
//   unpreviewed scene and restores happen in a safe order.
// - Owner: T0 (API); T3a implements it (the registry), makes the clip controller acquire it and
//   joins the Pose window (PoseEditSession acquires when it first writes bones and its stop is
//   goIdle); T2c makes the Inspector's action preview acquire it (InspectorWindow.cpp is T2c's);
//   T3b the Animation window's entity-mode preview.
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
        // > 0 for a granted ownership; 0 is never a valid token (no owner / not granted)
        using Token = int;
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

        // ---- additions (T3a) ----

        // acquire() / release() keyed by the owning object, for owners with no place to keep a
        // token (PoseEditSession): one ownership per `who`; acquiring again for the same root
        // keeps it (nobody is stopped), for another root releases the old one first
        static Token acquireFor(const void* who, uint32_t sceneId, Entity root, EntityRegistry* registry,
                                const std::string& name, Stop stop);
        static void releaseFor(const void* who);
        // the token `who` holds, 0 when none
        static Token tokenOf(const void* who);
        // `a` and `b` (of one scene) are the same entity or one is an ancestor of the other by
        // the Transform parents of `registry` (null: equality only)
        static bool overlaps(EntityRegistry* registry, Entity a, Entity b);
    };

}
