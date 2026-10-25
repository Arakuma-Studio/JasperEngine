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
// - Owner: T0 (API, the component-write helpers and the channel write CALL SITES, already in
//   place: PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticePointsCmd, execute and
//   undo; the morph weights have no editor write site yet: whoever adds one, e.g. T3c's Inspector
//   morph rows, calls before/afterComponentWrite); T3a implements the registries and the Animation
//   window's listeners; T3c calls claims/record at the sources (SceneRender gizmo drag, Inspector
//   Transform rows, morph rows, LatticeEditorOps) and moved the Pose window from
//   poseInspectorHooks().beforeTransformWrite onto beforeChannelWrite (the old slot is gone).
// - Thread-safety: main thread.
// - Invariants: listeners run in registration order; a listener may not add or remove listeners
//   while it runs (removal is deferred until the call returns); MCP set_properties never records
//   (its call sites do not ask claims()). A write notifies once per channel it writes: a Transform
//   property write ("position") one channel, a whole-Transform write (gizmo, paste, reset) three
//   (Position, Rotation, Scale), a lattice offsets write Lattice, a whole MeshComponent write
//   Morph; before* runs before the value changes, after* once it changed (also for undo/redo).
//   Listeners only write values of existing components (never add or remove components or
//   entities): the call sites hold component pointers across the call.

#pragma once

#include "animation/AnimationClip.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Catalog.h
    enum class ComponentType : int;

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

        // The channels a component write touches (implemented): Transform "position" / "rotation"
        // / "scale" that one, "" (the whole component) all three, other Transform properties none;
        // LatticeComponent "offsets" or "" Lattice; MeshComponent "" Morph; anything else none
        static std::vector<ClipChannel> channelsOf(ComponentType type, const std::string& property);
        // before/afterChannelWrite for every channel of channelsOf (the command call sites)
        static void beforeComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property);
        static void afterComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property);

        // ---- additions (T3c): helpers of the record sources ----

        // Record takes this edit of a selection: some of `entities` is claimed for `channel`. The
        // sources then record the claimed ones and leave the others unchanged (no scene step: one
        // edit is never split between the project and the scene history; the Animation window's
        // status says which selected GameObjects Record leaves alone)
        static bool claimsAny(uint32_t sceneId, const std::vector<Entity>& entities, ClipChannel channel);
        // the record value of a Transform channel (position / scale x y z, rotation x y z w)
        static ClipRecordValue transformValue(ClipChannel channel, const Vector3& position, const Quaternion& rotation, const Vector3& scale);
        // the merge key of a source's gesture: the current drag's, or a new drag's (then `opened`
        // is set and the source calls endDrag() when the gesture ends: release, deactivation)
        static std::string gestureMergeKey(bool* opened);
    };

}
