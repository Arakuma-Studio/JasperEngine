// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.4, 9.R5/R6)
// - Purpose: the interception points of Record mode and of preview snapshots, at the sources of
//   scene edits (before any command is built):
//   * record listeners: claims(scene, entity, channel) / record(...). A claimed edit is written
//     as the live preview value and keyed into the clip (merge key "record#<dragSerial>"), no
//     scene command; unclaimed edits stay scene steps.
//   * channel write listeners: beforeChannelWrite / afterChannelWrite around every scene command,
//     undo or redo that writes a Transform / morph weights / lattice offsets of an entity (today
//     PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticeEditorOps): a previewing owner
//     puts the entity back to its rest before (so the command records and edits the rest) and
//     takes the new value as its snapshot after (then re-applies its preview).
//   This replaces the single poseInspectorHooks().beforeTransformWrite slot: both the Pose window
//   and the Animation window are listeners.
// - Owner: T0 (API); T3a implements the registry and the Animation window's listeners; T3c calls
//   claims/record at the sources (SceneRender gizmo drag, Inspector Transform rows, MeshComponent
//   morph rows, LatticeEditorOps::push) and moves the Pose window and the existing
//   beforeTransformWrite call sites onto beforeChannelWrite.
// - Thread-safety: main thread.
// - Invariants: listeners run in registration order; a listener may not add or remove listeners
//   while it runs (removal is deferred until the call returns); MCP set_properties never records
//   (its call sites do not ask claims()).

#pragma once

#include "animation/AnimationClip.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // A value as Record writes it: the channel's flattened key value (ClipKey::value layout:
    // position/scale 3, rotation 4 x y z w, morph one per target, lattice 3 per point of `points`)
    struct ClipRecordValue{
        ClipChannel channel = ClipChannel::Position;
        std::vector<float> value;
        std::vector<int> points;            // lattice only: the flat indices `value` lists
    };

    struct ClipRecordListener{
        // the listener keys edits of this channel of this entity (Record is on, entity bound)
        std::function<bool(uint32_t sceneId, Entity entity, ClipChannel channel)> claims;
        // writes the live value and commits the key at the playhead
        std::function<void(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey)> record;
    };

    using ChannelWriteListener = std::function<void(uint32_t sceneId, Entity entity, ClipChannel channel)>;

    class ClipRecordHooks{
    public:
        using Id = int;     // 0 = none

        static Id addRecordListener(ClipRecordListener listener);
        static void removeRecordListener(Id id);
        // some listener claims it
        static bool claims(uint32_t sceneId, Entity entity, ClipChannel channel);
        // the first claiming listener records it; false when none claimed (build the scene command)
        static bool record(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey);

        // A drag or widget edit started (gizmo press, Inspector item activated): the serial its
        // record() calls share, "record#<serial>"; endDrag() on release / deactivation
        static std::string beginDrag();
        static void endDrag();
        // the current drag's merge key, "" between drags (discrete edits do not merge)
        static std::string currentMergeKey();

        static Id addBeforeChannelWrite(ChannelWriteListener listener);
        static Id addAfterChannelWrite(ChannelWriteListener listener);
        static void removeChannelWriteListener(Id id);
        static void beforeChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel);
        static void afterChannelWrite(uint32_t sceneId, Entity entity, ClipChannel channel);
    };

}
