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

        // ---- owners without a token of their own, overlap and stops before a copy ----

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
        // Before something copies or stores entities of `sceneId` (duplicate, copy, delete, create
        // or apply a prefab: the PoseEditSession::suspendActive* sites): every owner whose root is
        // one of `roots`, under one or above one stops (restores), except the one held by `except`
        // (acquireFor's `who`). Empty `roots`: every owner of the scene; sceneId 0: of every
        // scene. `registry` null: each owner's own (the one it acquired with).
        static void stopFor(uint32_t sceneId, const std::vector<Entity>& roots, EntityRegistry* registry,
                            const void* except = nullptr);

        // ---- what a preview writes ----

        // What an owner's preview writes now (an entity whose own values it shows): every owner
        // sets one when it acquires. A step that only touches something new under a previewed
        // root (a first model load) asks writes() instead of stopping the root's preview.
        using Writes = std::function<bool(Entity entity)>;
        static void setWrites(Token token, Writes writes);
        // some owner of `sceneId` previews `entity`'s own values; an owner without a predicate
        // answers for its root entity only
        static bool writes(uint32_t sceneId, Entity entity);
    };

}
