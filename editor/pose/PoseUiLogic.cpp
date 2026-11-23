// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseUiLogic.h"

#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"
#include "pose/PoseRig.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

using namespace jasperengine;

namespace editor = jasperengine::editor;
namespace poseui = jasperengine::editor::poseui;

namespace {

    bool isSeparator(char c){
        return c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']';
    }

    // "left" -> "right" keeping the case pattern (Left -> Right, LEFT -> RIGHT, left -> right)
    std::string swapWord(const std::string& word, const std::string& replacement){
        bool allUpper = word.size() > 1;
        for (char c : word){
            if (!std::isupper(static_cast<unsigned char>(c))) allUpper = false;
        }
        std::string out = replacement;
        if (allUpper){
            for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }else if (std::isupper(static_cast<unsigned char>(word[0]))){
            out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        }
        return out;
    }

    std::string lower(std::string text){
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string fixed2(double value){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
        return buffer;
    }

    std::vector<int> sortedUnique(std::vector<int> indices, int count){
        indices.erase(std::remove_if(indices.begin(), indices.end(), [count](int i){ return i < 0 || i >= count; }), indices.end());
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }

    poseui::Edit unchanged(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
        poseui::Edit edit;
        edit.data = data;
        edit.selection = selected;
        edit.primary = primary;
        return edit;
    }

    bool sameKey(const pose::PoseKey& a, const pose::PoseKey& b){
        return a.name == b.name && a.hold == b.hold && a.customTransition == b.customTransition && a.grounded == b.grounded &&
               a.lift == b.lift && a.body.angles == b.body.angles && a.body.offset == b.body.offset &&
               a.transition.curve == b.transition.curve && a.transition.duration == b.transition.duration &&
               a.transition.overshoot == b.transition.overshoot && a.transition.anticipation == b.transition.anticipation;
    }

    // An edit applied to every selected pose (the selection stays); unchanged when no pose changed
    poseui::Edit eachSelected(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary,
                              const std::function<void(pose::PoseKey&)>& change){
        poseui::Edit edit = unchanged(data, selected, primary);
        for (int index : sortedUnique(selected, (int)data.poses.size())){
            change(edit.data.poses[index]);
            if (!sameKey(edit.data.poses[index], data.poses[index])) edit.changed = true;
        }
        return edit;
    }

}

// ---- names and labels ----

std::string poseui::mirrorPoseName(const std::string& name){
    std::string out;
    size_t i = 0;
    while (i < name.size()){
        if (isSeparator(name[i])){
            out += name[i++];
            continue;
        }
        // a word: up to the next separator or a lower-to-upper case change ("LeftKick")
        size_t j = i + 1;
        while (j < name.size() && !isSeparator(name[j]) &&
               !(std::islower(static_cast<unsigned char>(name[j - 1])) && std::isupper(static_cast<unsigned char>(name[j])))){
            j++;
        }
        const std::string word = name.substr(i, j - i);
        const std::string key = lower(word);
        if (key == "left"){
            out += swapWord(word, "right");
        }else if (key == "right"){
            out += swapWord(word, "left");
        }else if (word == "L" || word == "R" || word == "l" || word == "r"){
            const bool upper = std::isupper(static_cast<unsigned char>(word[0])) != 0;
            out += (key == "l") ? (upper ? "R" : "r") : (upper ? "L" : "l");
        }else{
            out += word;
        }
        i = j;
    }
    return out;
}

std::string poseui::newPoseName(const std::vector<pose::PoseKey>& poses){
    std::set<std::string> used;
    for (const pose::PoseKey& key : poses) used.insert(key.name);
    for (size_t n = poses.size() + 1;; n++){
        std::string candidate = "Pose " + std::to_string(n);
        if (!used.count(candidate)) return candidate;
    }
}

std::string poseui::poseCountLabel(const std::string& verb, int count){
    if (count <= 1) return verb + " Pose";
    return verb + " " + std::to_string(count) + " Poses";
}

std::string poseui::transportLabel(double length, int poseCount, bool previewing, double time, int frame){
    const std::string poses = std::to_string(poseCount) + (poseCount == 1 ? " pose" : " poses");
    if (previewing){
        return fixed2(time) + " / " + fixed2(length) + " s \xC2\xB7 frame " + std::to_string(frame);
    }
    return fixed2(length) + " s \xC2\xB7 " + poses;
}

std::string poseui::holdLabel(float hold){
    return "hold " + fixed2(hold) + "s";
}

std::string poseui::transitionChipLabel(float duration, bool loopBack){
    return std::string(loopBack ? "\xE2\x86\xBA " : "\xE2\x86\x92 ") + fixed2(duration) + "s";
}

poseui::RigStatus poseui::rigStatus(const std::array<std::string, pose::HUMAN_BONE_COUNT>& bones){
    RigStatus status;
    std::vector<std::string> requiredMissing;
    std::vector<std::string> optionalMissing;
    int mapped = 0;
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        if (!bones[b].empty()){
            mapped++;
        }else if (pose::humanBoneInfo(pose::boneAt(b)).required){
            requiredMissing.push_back(pose::humanBoneName(pose::boneAt(b)));
        }else{
            optionalMissing.push_back(pose::humanBoneName(pose::boneAt(b)));
        }
    }
    auto join = [](const std::vector<std::string>& names){
        std::string text;
        for (const std::string& name : names){
            if (!text.empty()) text += ", ";
            text += name;
        }
        return text;
    };
    if (!requiredMissing.empty()){
        status.warning = true;
        status.text = "Missing: " + join(requiredMissing);
        status.tooltip = "Required bones without a mapping: " + join(requiredMissing) + ". Use Map Bones... to pick them.";
        if (!optionalMissing.empty()) status.tooltip += "\nOptional, not mapped: " + join(optionalMissing);
        return status;
    }
    status.text = "Humanoid " + std::to_string(mapped) + "/" + std::to_string(pose::HUMAN_BONE_COUNT);
    status.tooltip = optionalMissing.empty() ? std::string("Every humanoid bone is mapped.")
                                             : "Optional, not mapped (their parents carry them): " + join(optionalMissing);
    return status;
}

// ---- selection ----

void poseui::selectPose(Selection& selection, int index, bool additive, bool range, int poseCount){
    if (index < 0 || index >= poseCount) return;
    if (range && selection.anchor >= 0 && selection.anchor < poseCount){
        const int from = std::min(selection.anchor, index);
        const int to = std::max(selection.anchor, index);
        std::vector<int> indices;
        if (additive) indices = selection.indices;
        for (int i = from; i <= to; i++) indices.push_back(i);
        selection.indices = sortedUnique(indices, poseCount);
        selection.primary = index;
        return;
    }
    if (additive){
        auto it = std::find(selection.indices.begin(), selection.indices.end(), index);
        if (it != selection.indices.end()){
            if (selection.indices.size() == 1) return;   // the last selected pose stays
            selection.indices.erase(it);
            if (selection.primary == index) selection.primary = selection.indices.back();
            selection.anchor = selection.primary;
            return;
        }
        selection.indices.push_back(index);
        selection.indices = sortedUnique(selection.indices, poseCount);
        selection.primary = index;
        selection.anchor = index;
        return;
    }
    selection.indices = {index};
    selection.primary = index;
    selection.anchor = index;
}

void poseui::clampSelection(Selection& selection, int poseCount){
    selection.indices = sortedUnique(selection.indices, poseCount);
    if (poseCount <= 0){
        selection = Selection();
        return;
    }
    if (selection.primary >= poseCount) selection.primary = poseCount - 1;
    if (selection.primary < 0) selection.primary = selection.indices.empty() ? 0 : selection.indices.front();
    if (std::find(selection.indices.begin(), selection.indices.end(), selection.primary) == selection.indices.end()){
        selection.indices.push_back(selection.primary);
        selection.indices = sortedUnique(selection.indices, poseCount);
    }
    if (selection.anchor < 0 || selection.anchor >= poseCount) selection.anchor = selection.primary;
}

bool poseui::isSelected(const Selection& selection, int index){
    return std::find(selection.indices.begin(), selection.indices.end(), index) != selection.indices.end();
}

// ---- strip reorder ----

std::vector<int> poseui::reorderForDrop(int count, const std::vector<int>& moving, int insertBefore){
    const std::vector<int> sel = sortedUnique(moving, count);
    if (sel.empty() || count <= 0) return {};
    insertBefore = std::clamp(insertBefore, 0, count);
    std::vector<int> rest;
    int insertAt = 0;
    for (int i = 0; i < count; i++){
        if (std::binary_search(sel.begin(), sel.end(), i)) continue;
        if (i < insertBefore) insertAt++;
        rest.push_back(i);
    }
    std::vector<int> order(rest.begin(), rest.begin() + insertAt);
    order.insert(order.end(), sel.begin(), sel.end());
    order.insert(order.end(), rest.begin() + insertAt, rest.end());
    for (int i = 0; i < count; i++){
        if (order[i] != i) return order;
    }
    return {};
}

std::vector<int> poseui::reorderForShift(int count, const std::vector<int>& moving, int direction){
    const std::vector<int> sel = sortedUnique(moving, count);
    if (sel.empty() || direction == 0) return {};
    if (direction < 0){
        if (sel.front() == 0) return {};
        return reorderForDrop(count, sel, sel.front() - 1);
    }
    if (sel.back() >= count - 1) return {};
    // after the pose that follows the last selected one: before the one after it
    return reorderForDrop(count, sel, sel.back() + 2);
}

std::vector<int> poseui::newPositions(const std::vector<int>& order, const std::vector<int>& oldIndices){
    std::vector<int> positions;
    for (int old : oldIndices){
        const int position = newPosition(order, old);
        if (position >= 0) positions.push_back(position);
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

int poseui::newPosition(const std::vector<int>& order, int oldIndex){
    for (size_t i = 0; i < order.size(); i++){
        if (order[i] == oldIndex) return (int)i;
    }
    return order.empty() ? oldIndex : -1;
}

void poseui::applyOrder(std::vector<pose::PoseKey>& poses, const std::vector<int>& order){
    if (order.size() != poses.size()) return;
    std::vector<pose::PoseKey> reordered;
    reordered.reserve(poses.size());
    for (int old : order) reordered.push_back(poses[old]);
    poses = std::move(reordered);
}

int poseui::insertionIndexAt(const std::vector<float>& centres, float x){
    int index = 0;
    for (float centre : centres){
        if (x < centre) break;
        index++;
    }
    return index;
}

bool poseui::dragStarted(float dx, float dy, float threshold){
    return dx * dx + dy * dy >= threshold * threshold;
}

// ---- pose edits ----

pose::PoseKey poseui::neutralPose(const std::vector<pose::PoseKey>& poses){
    pose::PoseKey key;
    key.name = newPoseName(poses);
    return key;
}

poseui::Edit poseui::duplicatePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, bool atEnd, bool mirrored){
    const std::vector<int> sel = sortedUnique(selected, (int)data.poses.size());
    Edit edit = unchanged(data, selected, sel.empty() ? -1 : sel.back());
    if (sel.empty()) return edit;
    std::vector<pose::PoseKey> copies;
    for (int index : sel){
        pose::PoseKey copy = data.poses[index];
        if (mirrored){
            copy.body = pose::mirrorBodyPose(copy.body);
            copy.name = mirrorPoseName(copy.name);
        }
        copies.push_back(std::move(copy));
    }
    const int insertAt = atEnd ? (int)data.poses.size() : sel.back() + 1;
    edit.data.poses.insert(edit.data.poses.begin() + insertAt, copies.begin(), copies.end());
    edit.selection.clear();
    for (int i = 0; i < (int)copies.size(); i++) edit.selection.push_back(insertAt + i);
    edit.primary = edit.selection.back();
    edit.changed = true;
    return edit;
}

poseui::Edit poseui::addPose(const pose::PoseAnimationData& data, int primary){
    if (data.poses.empty()){
        Edit edit = unchanged(data, {}, -1);
        edit.data.poses.push_back(neutralPose(data.poses));
        edit.selection = {0};
        edit.primary = 0;
        edit.changed = true;
        return edit;
    }
    if (primary < 0 || primary >= (int)data.poses.size()) primary = (int)data.poses.size() - 1;
    return duplicatePoses(data, {primary}, false, false);
}

poseui::Edit poseui::deletePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    const std::vector<int> sel = sortedUnique(selected, (int)data.poses.size());
    Edit edit = unchanged(data, selected, primary);
    if (sel.empty()) return edit;
    for (auto it = sel.rbegin(); it != sel.rend(); ++it){
        edit.data.poses.erase(edit.data.poses.begin() + *it);
    }
    edit.changed = true;
    const int count = (int)edit.data.poses.size();
    if (count == 0){
        edit.selection.clear();
        edit.primary = -1;
        return edit;
    }
    // the pose that took the place of the first deleted one (or the new last pose)
    edit.primary = std::min(sel.front(), count - 1);
    edit.selection = {edit.primary};
    return edit;
}

poseui::Edit poseui::mirrorPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    return eachSelected(data, selected, primary, [](pose::PoseKey& key){ key.body = pose::mirrorBodyPose(key.body); });
}

poseui::Edit poseui::pastePose(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary, const pose::PoseKey& source){
    return eachSelected(data, selected, primary, [&source](pose::PoseKey& key){
        key.body = source.body;
        key.grounded = source.grounded;
        key.lift = source.lift;
    });
}

poseui::Edit poseui::movePoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary, const std::vector<int>& order){
    Edit edit = unchanged(data, selected, primary);
    if (order.size() != data.poses.size()) return edit;
    applyOrder(edit.data.poses, order);
    edit.selection = newPositions(order, sortedUnique(selected, (int)data.poses.size()));
    edit.primary = newPosition(order, primary);
    edit.changed = true;
    return edit;
}

poseui::Edit poseui::resetToNeutral(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    return eachSelected(data, selected, primary, [](pose::PoseKey& key){ key.body = pose::BodyPose(); });
}

poseui::Edit poseui::groundPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    return eachSelected(data, selected, primary, [](pose::PoseKey& key){
        key.grounded = true;
        key.lift = 0.0f;
    });
}

poseui::Edit poseui::centerPoses(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    return eachSelected(data, selected, primary, [](pose::PoseKey& key){ key.body.offset = Vector3(0, 0, 0); });
}

poseui::Edit poseui::resetCustomTransitions(const pose::PoseAnimationData& data, const std::vector<int>& selected, int primary){
    std::vector<int> all;
    for (int i = 0; i < (int)data.poses.size(); i++) all.push_back(i);
    Edit edit = eachSelected(data, all, primary, [](pose::PoseKey& key){ key.customTransition = false; });
    edit.selection = selected;
    return edit;
}

// ---- multi-selection values ----

poseui::MixedValue poseui::mixedValue(const pose::PoseAnimationData& data, const std::vector<int>& selected,
                                      const std::function<float(const pose::PoseKey&)>& get, float epsilon){
    MixedValue result;
    for (int index : sortedUnique(selected, (int)data.poses.size())){
        const float value = get(data.poses[index]);
        if (!result.any){
            result.value = value;
            result.any = true;
        }else if (std::fabs(value - result.value) > epsilon){
            result.mixed = true;
        }
    }
    return result;
}

bool poseui::setOnSelection(pose::PoseAnimationData& data, const std::vector<int>& selected, const std::function<void(pose::PoseKey&)>& set){
    if (!set) return false;
    bool changed = false;
    for (int index : sortedUnique(selected, (int)data.poses.size())){
        const pose::PoseKey before = data.poses[index];
        set(data.poses[index]);
        if (!sameKey(before, data.poses[index])) changed = true;
    }
    return changed;
}

poseui::MixedValue poseui::mixedAngle(const pose::PoseAnimationData& data, const std::vector<int>& selected, int dof){
    if (dof < 0 || dof >= pose::DOF_COUNT) return MixedValue();
    return mixedValue(data, selected, [dof](const pose::PoseKey& key){ return key.body.angles[dof]; }, 1e-4f);
}

bool poseui::setAngle(pose::PoseAnimationData& data, const std::vector<int>& selected, int dof, float degrees){
    if (dof < 0 || dof >= pose::DOF_COUNT) return false;
    const float wrapped = pose::wrapAngle(degrees);
    return setOnSelection(data, selected, [dof, wrapped](pose::PoseKey& key){ key.body.angles[dof] = wrapped; });
}

const std::vector<poseui::DofGroup>& poseui::dofGroups(){
    static const std::vector<DofGroup> groups = {
        {"Body", {pose::HumanBone::Hips}},
        {"Spine", {pose::HumanBone::Spine, pose::HumanBone::Chest, pose::HumanBone::UpperChest}},
        {"Head", {pose::HumanBone::Neck, pose::HumanBone::Head}},
        {"Left arm", {pose::HumanBone::LeftShoulder, pose::HumanBone::LeftUpperArm, pose::HumanBone::LeftLowerArm, pose::HumanBone::LeftHand}},
        {"Right arm", {pose::HumanBone::RightShoulder, pose::HumanBone::RightUpperArm, pose::HumanBone::RightLowerArm, pose::HumanBone::RightHand}},
        {"Left leg", {pose::HumanBone::LeftUpperLeg, pose::HumanBone::LeftLowerLeg, pose::HumanBone::LeftFoot, pose::HumanBone::LeftToes}},
        {"Right leg", {pose::HumanBone::RightUpperLeg, pose::HumanBone::RightLowerLeg, pose::HumanBone::RightFoot, pose::HumanBone::RightToes}},
    };
    return groups;
}

bool poseui::keepsGlobalEdit(size_t posesBefore, size_t posesAfter, bool reordered, int primaryBefore, int primaryAfter){
    return posesBefore == posesAfter && !reordered && primaryBefore == primaryAfter;
}

bool poseui::outsideLimits(int dof, float degrees){
    if (dof < 0 || dof >= pose::DOF_COUNT) return false;
    const pose::DofInfo& info = pose::dofInfo(dof);
    return degrees < info.minDegrees - 1e-3f || degrees > info.maxDegrees + 1e-3f;
}

// ---- feel ----

int poseui::matchingPresetIndex(const pose::PoseAnimationSettings& settings){
    const std::vector<pose::FeelPreset>& presets = pose::feelPresets();
    for (size_t i = 0; i < presets.size(); i++){
        if (pose::feelPresetMatches(presets[i], settings)) return (int)i;
    }
    return -1;
}

// ---- timing bar ----

poseui::TimingLayout poseui::timingLayout(const pose::PoseAnimationData& data){
    TimingLayout layout;
    if (data.poses.empty()) return layout;
    const pose::PoseTimeline timeline = buildTimeline(data);
    layout.length = timeline.length;
    const int count = (int)data.poses.size();
    // pose i's key time: 0 for pose 0, else the end of the segment that arrives at it
    double key = 0.0;
    for (int i = 0; i < count; i++){
        TimingBlock hold;
        hold.pose = i;
        hold.start = key;
        hold.end = key + timeline.holds[i];
        layout.holds.push_back(hold);
        if (i < (int)timeline.segmentCount()){
            TimingBlock transition;
            transition.pose = i;
            transition.start = timeline.start[i];
            transition.end = timeline.start[i] + timeline.transitions[i].duration;
            transition.loopBack = (i == count - 1);
            transition.custom = data.poses[i].customTransition;
            layout.transitions.push_back(transition);
            layout.curves.push_back(timeline.transitions[i]);
            key = transition.end;
        }else{
            key = hold.end;
        }
    }
    // one pose: the length is at least one frame (the hold may be 0)
    if (count == 1 && layout.holds[0].end < layout.length) layout.holds[0].end = layout.length;
    return layout;
}

poseui::TimingHit poseui::timingHitTest(const TimingLayout& layout, double x, double pixelsPerSecond, double tolerance){
    TimingHit hit;
    if (pixelsPerSecond <= 0.0 || layout.holds.empty()) return hit;
    double best = tolerance + 1e-9;
    // hold edges first: a transition edge must be strictly nearer to win
    for (const TimingBlock& hold : layout.holds){
        const double distance = std::fabs(hold.end * pixelsPerSecond - x);
        if (distance <= best){
            best = distance;
            hit.kind = TimingHitKind::HoldEdge;
            hit.pose = hold.pose;
        }
    }
    for (const TimingBlock& transition : layout.transitions){
        const double distance = std::fabs(transition.end * pixelsPerSecond - x);
        if (distance < best - 1e-9){
            best = distance;
            hit.kind = TimingHitKind::TransitionEdge;
            hit.pose = transition.pose;
        }
    }
    if (hit.kind != TimingHitKind::None) return hit;
    const double time = x / pixelsPerSecond;
    for (const TimingBlock& hold : layout.holds){
        if (time >= hold.start && time < hold.end){
            hit.kind = TimingHitKind::Hold;
            hit.pose = hold.pose;
            return hit;
        }
    }
    for (const TimingBlock& transition : layout.transitions){
        if (time >= transition.start && time < transition.end){
            hit.kind = TimingHitKind::Transition;
            hit.pose = transition.pose;
            return hit;
        }
    }
    return hit;
}

int poseui::poseAtTime(const TimingLayout& layout, double time){
    for (const TimingBlock& hold : layout.holds){
        if (time >= hold.start && time < hold.end) return hold.pose;
    }
    for (const TimingBlock& transition : layout.transitions){
        if (time >= transition.start && time < transition.end) return transition.pose;
    }
    if (!layout.holds.empty() && time >= layout.length && time <= layout.length + 1e-6){
        return layout.transitions.empty() || !layout.transitions.back().loopBack ? layout.holds.back().pose : layout.transitions.back().pose;
    }
    return -1;
}

float poseui::timingSnapStep(const pose::PoseAnimationSettings& settings){
    if (settings.stepRate > 0.0f) return 1.0f / settings.stepRate;
    return 1.0f / static_cast<float>(std::max(1, settings.frameRate));
}

float poseui::snapTimingValue(float value, float step, bool snapping, float min, float max){
    if (!std::isfinite(value)) value = min;
    if (snapping && step > 0.0f){
        float snapped = std::round(value / step) * step;
        if (snapped < min - 1e-6f) snapped = std::ceil(min / step - 1e-4f) * step;
        if (snapped > max + 1e-6f) snapped = std::floor(max / step + 1e-4f) * step;
        value = snapped;
    }
    return std::clamp(value, min, max);
}

float poseui::timingEdgeValue(const pose::PoseAnimationData& data, const TimingHit& hit){
    if (hit.pose < 0 || hit.pose >= (int)data.poses.size()) return 0.0f;
    if (hit.kind == TimingHitKind::HoldEdge) return data.poses[hit.pose].hold;
    if (hit.kind == TimingHitKind::TransitionEdge) return pose::transitionOut(data, hit.pose).duration;
    return 0.0f;
}

bool poseui::applyTimingDrag(pose::PoseAnimationData& data, const TimingHit& hit, float startValue, float deltaSeconds, bool snapping){
    if (hit.pose < 0 || hit.pose >= (int)data.poses.size()) return false;
    const float step = timingSnapStep(data.settings);
    pose::PoseKey& key = data.poses[hit.pose];
    if (hit.kind == TimingHitKind::HoldEdge){
        const float value = snapTimingValue(startValue + deltaSeconds, step, snapping, pose::ranges::HOLD_MIN, pose::ranges::HOLD_MAX);
        if (value == key.hold) return false;
        key.hold = value;
        return true;
    }
    if (hit.kind == TimingHitKind::TransitionEdge){
        const float value = snapTimingValue(startValue + deltaSeconds, step, snapping, pose::ranges::DURATION_MIN, pose::ranges::DURATION_MAX);
        if (key.customTransition && value == key.transition.duration) return false;
        if (!key.customTransition){
            key.transition = data.settings.transition;
            key.customTransition = true;
        }
        key.transition.duration = value;
        return true;
    }
    return false;
}

std::vector<float> poseui::easeSamples(const pose::Transition& transition, int count, float xMax){
    std::vector<float> values;
    if (count < 2) count = 2;
    for (int i = 0; i < count; i++){
        values.push_back(pose::ease(xMax * static_cast<float>(i) / static_cast<float>(count - 1), transition));
    }
    return values;
}

double poseui::frameTickStep(double pixelsPerSecond, int frameRate, double minPixels){
    if (frameRate <= 0 || pixelsPerSecond <= 0.0) return 0.0;
    const double step = 1.0 / frameRate;
    return step * pixelsPerSecond >= minPixels ? step : 0.0;
}

// ---- Global Edit ----

int poseui::applyGlobalEditDelta(const pose::BodyPose& base, const pose::BodyPose& current, pose::BodyPose& target,
                                 std::vector<int>* changedSlots, float thresholdDegrees){
    int changed = 0;
    if (changedSlots) changedSlots->clear();
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const pose::HumanBone bone = pose::boneAt(b);
        const float* baseAngles = &base.angles[b * pose::DOFS_PER_BONE];
        const float* currentAngles = &current.angles[b * pose::DOFS_PER_BONE];
        const pose::Quatd eBase = jointRotation(bone, baseAngles);
        const pose::Quatd eCurrent = jointRotation(bone, currentAngles);
        const pose::Quatd delta = (eCurrent * eBase.inverse()).normalized();
        if (pose::radToDeg(pose::angleBetween(pose::Quatd(), delta)) <= thresholdDegrees) continue;
        const pose::Quatd eTarget = jointRotation(bone, &target.angles[b * pose::DOFS_PER_BONE]);
        pose::jointAngles(bone, (delta * eTarget).normalized(), &target.angles[b * pose::DOFS_PER_BONE]);
        changed++;
        if (changedSlots) changedSlots->push_back(b);
    }
    return changed;
}

// ---- Templates tab ----

std::string poseui::templateSummary(size_t poseCount, bool loop){
    return std::to_string(poseCount) + (poseCount == 1 ? " pose" : " poses") + " \xC2\xB7 " + (loop ? "loop" : "once");
}

std::vector<std::string> poseui::templateCategories(const std::vector<editor::PoseTemplateInfo>& templates){
    std::vector<std::string> categories;
    for (const editor::PoseTemplateInfo& info : templates){
        if (info.category.empty()) continue;
        const std::string key = lower(info.category);
        if (std::none_of(categories.begin(), categories.end(), [&key](const std::string& c){ return lower(c) == key; })){
            categories.push_back(info.category);
        }
    }
    return categories;
}

bool poseui::templateMatches(const editor::PoseTemplateInfo& info, const std::string& category, const std::string& search){
    if (!category.empty() && lower(info.category) != lower(category)) return false;
    const std::string haystack = lower(info.name + "\n" + info.category + "\n" + info.description);
    const std::string needle = lower(search);
    size_t at = 0;
    while (at < needle.size()){
        const size_t start = needle.find_first_not_of(" \t", at);
        if (start == std::string::npos) break;
        size_t end = needle.find_first_of(" \t", start);
        if (end == std::string::npos) end = needle.size();
        if (haystack.find(needle.substr(start, end - start)) == std::string::npos) return false;
        at = end;
    }
    return true;
}

std::string poseui::templateAnimationPath(const std::string& directory, const std::string& templateName,
                                          const std::function<bool(const std::string&)>& exists){
    std::string folder = directory;
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
    if (folder == ".") folder.clear();
    const std::string stem = editor::PoseTemplates::fileStem(templateName);
    std::string path;
    for (int n = 0; n < 1000; n++){
        const std::string name = n == 0 ? stem : stem + " " + std::to_string(n);
        path = (folder.empty() ? std::string() : folder + "/") + name + ".poseanim";
        if (!exists || !exists(path)) break;
    }
    return path;
}

poseui::Edit poseui::applyTemplate(const pose::PoseAnimationData& data, const pose::PoseAnimationData& templateData,
                                   editor::PoseTemplateApply mode, const std::vector<int>& selected, int primary){
    Edit edit;
    edit.data = data;
    editor::PoseTemplates::apply(templateData, edit.data, mode);
    if (mode == editor::PoseTemplateApply::Append && templateData.poses.empty()){
        // nothing to add
        edit.selection = selected;
        edit.primary = primary;
        return edit;
    }
    edit.changed = true;
    if (mode == editor::PoseTemplateApply::Replace){
        if (!edit.data.poses.empty()){
            edit.selection = {0};
            edit.primary = 0;
        }
    }else{
        const int first = static_cast<int>(data.poses.size());
        for (int i = first; i < static_cast<int>(edit.data.poses.size()); i++) edit.selection.push_back(i);
        edit.primary = first;
    }
    return edit;
}
