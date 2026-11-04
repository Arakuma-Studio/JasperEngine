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
//   place: PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticePointsCmd and
//   MorphWeightsCmd (the Inspector's Blend Shapes rows), execute and undo; a new write site of a
//   channel calls before/afterComponentWrite too); T3a implements the registries and the Animation
//   window's listeners; T3c calls claims/record at the sources (SceneRender gizmo drag, Inspector
//   Transform rows, morph rows, LatticeEditorOps) and moved the Pose window from
//   poseInspectorHooks().beforeTransformWrite onto beforeChannelWrite (the old slot is gone).
// - Thread-safety: main thread.
// - Invariants: listeners run in registration order; a listener may not add or remove listeners
//   while it runs (removal is deferred until the call returns); MCP set_properties never records
//   (its call sites do not ask claims()). A write notifies once per channel it writes: a Transform
//   property write ("position") one channel, a whole-Transform write (gizmo, paste, reset) three
//   (Position, Rotation, Scale), a lattice offsets write Lattice, a whole MeshComponent write
//   Morph and Property (every property of the component: Reset, Paste Component Values);
//   before* runs before the value changes, after* once it changed (also for undo/redo).
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

    // An animatable property as a track names it (anim-properties spec 5.1): the Catalog
    // component key ("ui", "light"), "gameobject" or "script:<Class>", and the whole property
    // (no swizzle)
    struct ClipPropertyKey{
        std::string component;
        std::string property;
        bool empty() const { return component.empty(); }
        bool operator==(const ClipPropertyKey& other) const { return component == other.component && property == other.property; }
        bool operator!=(const ClipPropertyKey& other) const { return !(*this == other); }
    };

    // A value as Record writes it: the channel's flattened key value (ClipKey::value layout:
    // position/scale 3, rotation 4 x y z w, morph one per target, lattice 3 per point of `points`;
    // a property its whole value in key space: sRGB colours, cone degrees, bools 0/1)
    struct ClipRecordValue{
        ClipChannel channel = ClipChannel::Position;
        std::vector<float> value;
        std::vector<int> points;            // lattice only: the flat indices `value` lists
        ClipPropertyKey property;           // channel Property only
    };

    struct ClipRecordListener{
        // the listener keys edits of this channel of this entity (Record is on, entity bound)
        std::function<bool(uint32_t sceneId, Entity entity, ClipChannel channel)> claims;
        // writes the live value and commits the key at the playhead
        std::function<void(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey)> record;
        // (T3c) the open clip has a track for this channel of this entity (the Inspector tints
        // animated rows stronger than rows Record would only start keying); optional
        std::function<bool(uint32_t sceneId, Entity entity, ClipChannel channel)> animates;
        // the same for an animatable property (channel Property): record() gets its values
        std::function<bool(uint32_t sceneId, Entity entity, const ClipPropertyKey& property)> claimsProperty;
        std::function<bool(uint32_t sceneId, Entity entity, const ClipPropertyKey& property)> animatesProperty;
    };

    // a scene write of an animatable property (PropertyCmd, the active and enabled checkboxes)
    using PropertyWriteListener = std::function<void(uint32_t sceneId, Entity entity, const ClipPropertyKey& property)>;

    // (T3c) who holds a record gesture: each source owns its own, so one source's open gesture
    // never merges (or records) another source's edits
    enum class RecordSource : int{
        Gizmo = 1,          // SceneRender's transform gizmo drag
        Inspector,          // the Inspector's Transform / Blend Shapes / lattice Offset rows
        LatticeDrag,        // the Scene view's lattice point drag (LatticeEditorOps continuous edit)
        Window              // the Animation window
    };

    using ChannelWriteListener = std::function<void(uint32_t sceneId, Entity entity, ClipChannel channel)>;

    // every component write a command makes (before/afterComponentWrite), animatable or not: the
    // entity Animation and Inspector action previews keep what a command wrote while they ran
    // (ActionPreviewSnapshot); property "" is the whole component
    using ComponentWriteListener = std::function<void(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property)>;

    class ClipRecordHooks{
    public:
        using Id = int;     // 0 = none

        static Id addRecordListener(ClipRecordListener listener);
        static void removeRecordListener(Id id);
        // some listener is installed (Record is on somewhere): the sources skip resolving their
        // rows' keys every frame otherwise
        static bool hasRecordListeners();
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
        // LatticeComponent "offsets" Lattice, "" Lattice and Property; MeshComponent
        // "morphWeights[i]" Morph, "" Morph and Property; a property of the animatable registry
        // (anim-properties: "ui" "color", "light" "intensity", "enabled" of a component with the
        // checkbox), a Lua script row ("scripts[N].<field>") or the whole of a component with
        // registry rows ("") Property; anything else none
        static std::vector<ClipChannel> channelsOf(ComponentType type, const std::string& property);
        // The animatable property a Catalog (component, property) write is (propertyOf), false
        // when none: the registry name (submesh material rows "submeshes[N]...", shader-library
        // members) or "enabled"; script properties go through scriptPropertyOf
        static bool propertyOf(ComponentType type, const std::string& property, ClipPropertyKey& out);
        // A Lua script's declared property, "script:<Class>" "<name>"
        static ClipPropertyKey scriptPropertyOf(const std::string& className, const std::string& name);
        static ClipPropertyKey activeProperty();    // "gameobject" "active"
        // The property key a component write notifies (before/afterComponentWrite): propertyOf's;
        // a whole-component write ("") the component with an empty property (every property of
        // it); a ScriptComponent write scriptRowComponent() with the Catalog row
        // ("scripts[N].<field>", "" for all), which listeners resolve on the GameObject (the
        // script's class). False when the write touches no animatable property
        static bool componentWriteKey(ComponentType type, const std::string& property, ClipPropertyKey& out);
        static const char* scriptRowComponent(){ return "scripts"; }
        // "scripts[N].<field>" split (false for other names)
        static bool scriptRow(const std::string& property, size_t& index, std::string& field);

        // ---- property record and writes (anim-properties spec 10) ----

        static bool claimsProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        static bool claimsAnyProperty(uint32_t sceneId, const std::vector<Entity>& entities, const ClipPropertyKey& property);
        static bool animatesProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        static Id addBeforePropertyWrite(PropertyWriteListener listener);
        static Id addAfterPropertyWrite(PropertyWriteListener listener);
        // (removed with removeChannelWriteListener)
        static void beforePropertyWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        static void afterPropertyWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        // before/afterChannelWrite for every channel of channelsOf (the command call sites); the
        // component write listeners first (before) and last (after)
        static void beforeComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property);
        static void afterComponentWrite(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property);
        // (removed with removeChannelWriteListener)
        static Id addBeforeComponentWrite(ComponentWriteListener listener);
        static Id addAfterComponentWrite(ComponentWriteListener listener);

        // ---- helpers of the record sources ----

        // Record takes this edit of a selection: some of `entities` is claimed for `channel`. The
        // sources then record the claimed ones and leave the others unchanged (no scene step: one
        // edit is never split between the project and the scene history; the Animation window's
        // status says which selected GameObjects Record leaves alone)
        static bool claimsAny(uint32_t sceneId, const std::vector<Entity>& entities, ClipChannel channel);
        // the record value of a Transform channel (position / scale x y z, rotation x y z w)
        static ClipRecordValue transformValue(ClipChannel channel, const Vector3& position, const Quaternion& rotation, const Vector3& scale);
        // the merge key of `source`'s gesture: its open one, or a new one (then `opened` is set and
        // the source calls endGesture(source) when the gesture ends: release, deactivation). Only
        // the same source's gesture is joined.
        static std::string gestureMergeKey(RecordSource source, bool* opened);
        static void endGesture(RecordSource source);
        static bool hasGesture(RecordSource source);
        // every source's gesture (Record stops, the preview stops): no gesture outlives Record
        static void endAllGestures();
        // some listener's `animates` says so
        static bool animates(uint32_t sceneId, Entity entity, ClipChannel channel);
        // A note for the Animation window's status (an edit Record does not key, e.g. a 2D or UI
        // move of a bound GameObject); takeNote returns it once ("" when none)
        static void note(const std::string& message);
        static std::string takeNote();

        // ---- explicit writes (FX) ----

        // A command's write of named values (PropertyCmd: an Inspector field, MCP set_properties
        // of a component's property; the Blend Shapes row's weight, a lattice point set by
        // index): those components of the channel value (the flattened ClipRecordValue layout)
        // are the edit, also when equal to what a clip preview shows, so the preview takes them
        // as the new rest. Components outside it follow the "left as shown" rule (a gizmo drag,
        // a whole-component write, the values a row carries as the Scene view shows them).
        struct ExplicitWrite{
            bool all = false;                   // every component of the channels written
            std::vector<size_t> components;
            bool any() const { return all || !components.empty(); }
            bool covers(size_t component) const;
        };
        // what the after-write being notified wrote explicitly (none outside an ExplicitWriteScope)
        static const ExplicitWrite& explicitWrite();
        // A preview shows other values than the scene's own for this channel now (a clip preview
        // wrote it): an explicit write equal to what is shown still changes the scene. Queries are
        // listeners like the channel writes (removed with removeChannelWriteListener).
        using PreviewQuery = std::function<bool(uint32_t sceneId, Entity entity, ClipChannel channel)>;
        static Id addPreviewQuery(PreviewQuery query);
        static bool previews(uint32_t sceneId, Entity entity, ClipChannel channel);
        // set around a command's afterComponentWrite / afterChannelWrite call
        class ExplicitWriteScope{
        public:
            explicit ExplicitWriteScope(ExplicitWrite write);
            ~ExplicitWriteScope();
            ExplicitWriteScope(const ExplicitWriteScope&) = delete;
            ExplicitWriteScope& operator=(const ExplicitWriteScope&) = delete;
        private:
            ExplicitWrite previous;
        };
    };

}
