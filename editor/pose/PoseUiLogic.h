// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pose Animation window's logic that needs no ImGui and no editor state (spec: pose
// animation phase 4, sections 3 and 4): pose name mirroring, labels, the timing bar's layout,
// hit testing, snapping and edge drags, the pose strip's selection and reorder index maths,
// the pose edits behind the strip's menu and shortcuts, feel preset matching, mixed values of
// a multi-selection, the Global Edit delta maths, and the Templates tab's filter, labels and
// edits (phase 6, section 4). Tested in jasperengine-pose-tests.

#pragma once

#include "pose/HumanBones.h"
#include "pose/PoseMath.h"
#include "pose/PoseTypes.h"
#include "pose/PoseTemplates.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::poseui{

    // ---- names and labels ----

    // Left/right swapped in a pose name: whole words Left/Right (any case: left, LEFT), single
    // letters L/R (and l/r) separated by a space, '_', '-', '.' or the ends of the name
    // ("Contact R" -> "Contact L", "Step.L" -> "Step.R", "Left Kick" -> "Right Kick").
    std::string mirrorPoseName(const std::string& name);
    // "Pose N" with the smallest N >= count + 1 that no pose uses
    std::string newPoseName(const std::vector<pose::PoseKey>& poses);

    // "Delete Pose" for one pose, "Delete 3 Poses" for several
    std::string poseCountLabel(const std::string& verb, int count);
    // "1.20 s · 6 poses" (one pose: "1 pose"); previewing: "0.42 / 1.20 s · frame 13"
    std::string transportLabel(double length, int poseCount, bool previewing, double time, int frame);
    std::string holdLabel(float hold);                       // "hold 0.10s"
    std::string transitionChipLabel(float duration, bool loopBack);   // "→ 0.12s", loop back "↺ 0.12s"

    // The rig status chip of the setup row
    struct RigStatus{
        std::string text;       // "Humanoid 22/22", "Missing: left_hand, right_hand"
        bool warning = false;   // a required slot is unmapped (the model cannot be posed)
        std::string tooltip;    // the unmapped slots, optional ones marked
    };
    // `bones`: the resolved humanoid map (slot -> bone, "" = unmapped)
    RigStatus rigStatus(const std::array<std::string, pose::HUMAN_BONE_COUNT>& bones);

    // ---- selection ----

    struct Selection{
        int primary = -1;              // the pose shown and posed
        int anchor = -1;               // where a Shift-click range starts
        std::vector<int> indices;      // sorted, unique, contains primary
    };
    // click: select only `index`; additive (Ctrl/Cmd): toggle it (the last selected pose stays);
    // range (Shift): from the anchor to `index`. `index` becomes the primary pose when selected.
    void selectPose(Selection& selection, int index, bool additive, bool range, int poseCount);
    // drops indices >= poseCount; an empty selection of a non-empty animation selects pose 0
    void clampSelection(Selection& selection, int poseCount);
    bool isSelected(const Selection& selection, int index);

    // ---- strip reorder ----

    // The new order (old index per new position) when the poses `moving` are dropped before
    // old index `insertBefore` (0..count; count = at the end), keeping their relative order.
    // Empty when nothing moves.
    std::vector<int> reorderForDrop(int count, const std::vector<int>& moving, int insertBefore);
    // Move Left (-1) / Right (+1) of the poses `moving` by one place; empty at the strip's end
    std::vector<int> reorderForShift(int count, const std::vector<int>& moving, int direction);
    // where old indices land in `order`
    std::vector<int> newPositions(const std::vector<int>& order, const std::vector<int>& oldIndices);
    int newPosition(const std::vector<int>& order, int oldIndex);
    void applyOrder(std::vector<pose::PoseKey>& poses, const std::vector<int>& order);
    // Insertion index (0..n) for a pointer at x over cards whose centres are `centres`
    int insertionIndexAt(const std::vector<float>& centres, float x);
    // a press becomes a drag after `threshold` pixels
    bool dragStarted(float dx, float dy, float threshold = 6.0f);

    // ---- pose edits (the strip's menu, the Pose tab's tools and the shortcuts) ----

    // The result of an edit: the new data, and the poses to select afterwards
    struct Edit{
        pose::PoseAnimationData data;
        std::vector<int> selection;
        int primary = -1;
        bool changed = false;
    };
    // a neutral pose (angles 0, grounded) named newPoseName
    pose::PoseKey neutralPose(const std::vector<pose::PoseKey>& poses);
    // copies of `selected` inserted after the last selected pose (or at the end), selected;
    // mirrored copies get mirrored bodies and L/R swapped names
    Edit duplicatePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, bool atEnd, bool mirrored);
    // "+": duplicates the primary pose after it and selects the copy; no poses: a neutral one
    Edit addPose(const pose::PoseAnimationData& data, int primary);
    Edit deletePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);
    Edit mirrorPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);
    // body, offset, grounded and lift from `source`; the targets keep their name, hold and transition
    Edit pastePose(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary, const pose::PoseKey& source);
    Edit movePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary, const std::vector<int>& order);
    Edit resetToNeutral(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);
    Edit groundPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);   // grounded, lift 0
    Edit centerPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);   // offset 0
    Edit resetCustomTransitions(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary);

    // ---- multi-selection values ----

    struct MixedValue{
        float value = 0.0f;     // the first selected pose's value
        bool mixed = false;     // the selected poses differ
        bool any = false;       // at least one pose is selected
    };
    MixedValue mixedValue(const pose::PoseAnimationData& data, const std::vector<int>& selected,
                          const std::function<float(const pose::PoseKey&)>& get, float epsilon = 1e-5f);
    // sets a value on every selected pose (returns true when one changed)
    bool setOnSelection(pose::PoseAnimationData& data, const std::vector<int>& selected,
                        const std::function<void(pose::PoseKey&)>& set);
    MixedValue mixedAngle(const pose::PoseAnimationData& data, const std::vector<int>& selected, int dof);
    bool setAngle(pose::PoseAnimationData& data, const std::vector<int>& selected, int dof, float degrees);

    // The Pose tab's joint-angle groups: Body (hips), Spine, Head, Left arm, Right arm, Left leg, Right leg
    struct DofGroup{
        const char* name;
        std::vector<pose::HumanBone> bones;
    };
    const std::vector<DofGroup>& dofGroups();
    // the joint limits of the 4.5 table (hips: none)
    bool outsideLimits(int dof, float degrees);

    // ---- feel ----

    // index into pose::feelPresets() of the preset the settings match, -1 for none
    int matchingPresetIndex(const pose::PoseAnimationSettings& settings);

    // ---- timing bar ----

    struct TimingBlock{
        int pose = -1;          // the pose of a hold; the pose a transition leaves
        double start = 0.0;     // seconds
        double end = 0.0;
        bool loopBack = false;  // the transition from the last pose back to pose 0
        bool custom = false;    // the pose's own transition
    };
    struct TimingLayout{
        double length = 0.0;
        std::vector<TimingBlock> holds;          // one per pose, in pose order
        std::vector<TimingBlock> transitions;    // one per segment (pose s -> s + 1)
        std::vector<pose::Transition> curves;    // the transition of each segment
    };
    TimingLayout timingLayout(const pose::PoseAnimationData& data);

    enum class TimingHitKind{ None, Hold, Transition, HoldEdge, TransitionEdge };
    struct TimingHit{
        TimingHitKind kind = TimingHitKind::None;
        int pose = -1;          // the pose that owns the hold / transition
    };
    // `x`: pixels from the bar's start. Edges within `tolerance` pixels win over bodies, the
    // nearest edge wins and a hold edge wins over a transition edge at the same x (so a zero
    // hold stays draggable).
    TimingHit timingHitTest(const TimingLayout& layout, double x, double pixelsPerSecond, double tolerance = 5.0);
    // the pose a time belongs to (a hold or the transition it leaves); -1 outside
    int poseAtTime(const TimingLayout& layout, double time);

    // The snapping grid of timing drags: 1 / stepRate when choppy, else 1 / frameRate
    float timingSnapStep(const pose::PoseAnimationSettings& settings);
    // value rounded to the grid (when snapping) and clamped to [min, max]; a value that rounds
    // below min takes the first grid step >= min
    float snapTimingValue(float value, float step, bool snapping, float min, float max);
    // The value an edge drag edits (a hold or the transition duration of `hit.pose`)
    float timingEdgeValue(const pose::PoseAnimationData& data, const TimingHit& hit);
    // Sets the dragged value to startValue + deltaSeconds (snapped, clamped); a transition edge
    // makes the pose's transition custom. True when the data changed.
    bool applyTimingDrag(pose::PoseAnimationData& data, const TimingHit& hit, float startValue, float deltaSeconds, bool snapping);
    // `count` ease values at x = xMax * i / (count - 1), for drawing a transition as its curve.
    // A spring first reaches the target at x = 1 and overshoots after it: xMax > 1 shows the
    // settle over the next pose's hold.
    std::vector<float> easeSamples(const pose::Transition& transition, int count = 24, float xMax = 1.0f);
    // seconds between frame ticks, 0 when they would be closer than `minPixels`
    double frameTickStep(double pixelsPerSecond, int frameRate, double minPixels = 6.0);

    // ---- the session's bone writes ----

    // When the edit session may write the bound model's bone transforms (phase 4 spec, section
    // 2: never while the scene is saved or played). suspend() before a save (sync or async) or
    // play mode; the bones stay untouched until resume() is called with the scene stopped
    // again (the session's per-frame update), whatever starts in between (posing, preview,
    // selection, document edits).
    struct BoneWriteGate{
        bool suspended = false;
        void suspend() { suspended = true; }
        // true when the session must apply its pose again now
        bool resume(bool sceneStopped){
            if (!suspended || !sceneStopped) return false;
            suspended = false;
            return true;
        }
        bool mayWrite(bool sceneStopped) const { return sceneStopped && !suspended; }
    };

    // Whether a window edit keeps an active Global Edit: edits that keep the pose count, the
    // order and the edited (primary) pose do (angles, offset, hold, transition, feel,
    // humanize); adding, deleting, reordering or selecting another pose cancels it
    bool keepsGlobalEdit(size_t posesBefore, size_t posesAfter, bool reordered, int primaryBefore, int primaryAfter);

    // ---- Global Edit ----


    // The change from `base` to `current` (one pose before and after an edit) applied to
    // `target`, per slot in joint-angle space: D_b = E_cur_b * E_base_b^-1 for every slot whose
    // change is above `thresholdDegrees`, E'_b = D_b * E_target_b (phase 4 spec, section 2).
    // E depends only on a slot's three DOF values (4.5), so this needs no rig and also carries
    // the slots a rig does not map. The target's offset is unchanged. Returns the number of
    // changed slots (0: nothing to apply); `changedSlots` (optional) receives them.
    int applyGlobalEditDelta(const pose::BodyPose& base, const pose::BodyPose& current,
                             pose::BodyPose& target, std::vector<int>* changedSlots = nullptr,
                             float thresholdDegrees = 0.01f);

    // ---- Templates tab (phase 6, section 4) ----

    // A card's second line: "4 poses · loop", "1 pose · once"
    std::string templateSummary(size_t poseCount, bool loop);
    // The categories of `templates` in list order, each once (compared without case)
    std::vector<std::string> templateCategories(const std::vector<PoseTemplateInfo>& templates);
    // The filter of the tab: `category` ("" = all) compared without case; every word of
    // `search` must occur (without case) in the name, the category or the description
    bool templateMatches(const PoseTemplateInfo& info, const std::string& category, const std::string& search);
    // The default file of a new animation from a template: `<directory>/<template file stem>.poseanim`
    // (directory project-relative, "" = the project root), " 1", " 2"... appended while `exists`
    // says the path is taken
    std::string templateAnimationPath(const std::string& directory, const std::string& templateName,
                                      const std::function<bool(const std::string&)>& exists);
    // Replace Poses in Current Animation / Append Poses as one edit (PoseTemplates::apply):
    // Replace selects pose 0, Append selects the appended poses (the first one primary)
    Edit applyTemplate(const pose::PoseAnimationData& data, const pose::PoseAnimationData& templateData,
                       PoseTemplateApply mode, const std::vector<int>& selected, int primary);

}
