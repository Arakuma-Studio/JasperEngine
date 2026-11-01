// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the Animation window's clip mode files (AnimationWindowClip*.cpp): metrics,
// colours (from Theme), labels, Euler conversion, sampled values, key insertion. Internal.

#pragma once

#include "Engine.h"
#include "Theme.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipEditorUtil.h"
#include "animation/KeyframeSampling.h"
#include "imgui.h"
#include "pose/PoseDofs.h"
#include "pose/PoseRig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace jasperengine::editor::clipui{

    constexpr float kKeyEpsilon = 1e-4f;
    constexpr float kMaxTime = 3600.0f;

    struct SheetMetrics{
        float s;
        float rulerHeight;
        float eventsHeight;
        float rowHeight;
        float valueWidth;
        float indent;
        float keyRadius;
        float keyHit;
        float timePad;      // time 0 sits this far right of the key area edge
        float playheadHalf;
        float eventWidth;
    };

    inline SheetMetrics sheetMetrics(){
        const float s = Theme::dpiScale();
        SheetMetrics m;
        m.s = s;
        m.rulerHeight = 22.0f * s;
        m.eventsHeight = 18.0f * s;
        m.rowHeight = ImGui::GetFrameHeight() + 2.0f * s;
        m.valueWidth = 104.0f * s;
        m.indent = 14.0f * s;
        m.keyRadius = 6.0f * s;
        m.timePad = 12.0f * s;
        m.keyHit = 7.5f * s;
        m.playheadHalf = 5.0f * s;
        m.eventWidth = 8.0f * s;
        return m;
    }

    inline ImU32 color(const ImVec4& c, float alpha = -1.0f){
        return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, alpha < 0.0f ? c.w : alpha));
    }

    inline ImVec4 mix(const ImVec4& a, const ImVec4& b, float t){
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    // the clip mode's colours, all from the Theme
    inline ImVec4 selectedKeyColor(){ return mix(Theme::Colors::Selection, ImVec4(1, 1, 1, 1), 0.35f); }
    inline ImVec4 playheadColor(){ return Theme::Colors::ErrorText; }
    inline ImVec4 pendingTextColor(){ return mix(Theme::Colors::ErrorText, ImVec4(1, 1, 1, 1), 0.3f); }
    inline ImVec4 pendingFieldColor(){ return mix(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), 0.55f); }
    inline ImVec4 keyColor(){ return mix(Theme::Colors::SubtleText, ImVec4(1, 1, 1, 1), 0.55f); }

    // (channelLabel, lastSegment: animation/ClipEditorUtil.h, shared with the Inspector)

    // a property track's (component, property) as one string ("ui.color.a"), "" for the others
    inline std::string propertyId(const ClipTrackData& track){
        return track.channel == ClipChannel::Property ? track.component + "." + track.property : std::string();
    }

    // a track's identity: path and channel, and a property track's component and property
    inline std::string rowKey(const ClipTrackData& track){
        // humanoid tracks have no path: their bone tells them apart
        if (isHumanoidChannel(track.channel)) return std::string("humanoid:") + anim::humanoidTrackBoneName(track.channel, track.humanBone) + "|" + anim::channelName(track.channel);
        const std::string key = track.path + "|" + anim::channelName(track.channel);
        return track.channel == ClipChannel::Property ? key + "|" + propertyId(track) : key;
    }

    // the track of an identity (path, channel, propertyId), -1 when none (a humanoid track's
    // path is its bone name)
    inline int findTrackById(const ClipData& data, const std::string& path, ClipChannel channel, const std::string& property){
        if (channel != ClipChannel::Property) return data.findTrack(path, channel);
        // "component.property": a component key (or script:<Class>) has no '.'
        const size_t dot = property.find('.');
        if (dot == std::string::npos) return -1;
        return data.findPropertyTrack(path, property.substr(0, dot), property.substr(dot + 1));
    }

    // the built clip's track of a data track (the same identity), -1 when none
    inline int builtTrack(const AnimationClip& clip, const ClipTrackData& track){
        return clipui::findRuntimeTrack(clip, track);
    }

    inline std::string lower(std::string s){
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
        return s;
    }

    // Euler degrees (the Inspector's ZYX order, the clip's {"euler"} convention) of an x y z w key
    inline Vector3 eulerOf(const std::vector<float>& xyzw){
        if (xyzw.size() < 4) return Vector3(0, 0, 0);
        Vector3 e = quaternionFromXYZW(xyzw.data()).normalize().getEulerAngles(RotationOrder::ZYX);
        if (!Engine::isUseDegrees()){
            const float k = 180.0f / 3.14159265358979323846f;
            e = Vector3(e.x * k, e.y * k, e.z * k);
        }
        auto tidy = [](float v){ return std::fabs(v) < 1e-4f ? 0.0f : v; };
        return Vector3(tidy(e.x), tidy(e.y), tidy(e.z));
    }

    inline std::vector<float> xyzwOf(const Vector3& eulerDegrees){
        std::vector<float> out(4);
        quaternionToXYZW(anim::eulerDegreesToQuaternion(eulerDegrees.x, eulerDegrees.y, eulerDegrees.z), out.data());
        return out;
    }

    // ---- how keys show as three angles (anim-retarget 9) ----
    // rotation tracks and humanoid extra bones: Euler degrees; humanoid slots: the slot's joint
    // angles (the pose system's DOFs: raise, forward, twist...), written back as E

    inline bool isAngleTrack(const ClipTrackData& track){
        return isRotationChannel(track.channel);
    }
    inline bool isJointAngleTrack(const ClipTrackData& track){
        return track.channel == ClipChannel::Humanoid && track.humanBone >= 0 && track.humanBone < pose::HUMAN_BONE_COUNT;
    }

    inline Vector3 anglesOf(const ClipTrackData& track, const std::vector<float>& xyzw){
        if (!isJointAngleTrack(track)) return eulerOf(xyzw);
        if (xyzw.size() < 4) return Vector3(0, 0, 0);
        float angles[pose::DOFS_PER_BONE];
        pose::jointAngles(pose::boneAt(track.humanBone), pose::Quatd(quaternionFromXYZW(xyzw.data())).normalized(), angles);
        auto tidy = [](float v){ return std::fabs(v) < 1e-4f ? 0.0f : v; };
        return Vector3(tidy(angles[0]), tidy(angles[1]), tidy(angles[2]));
    }

    inline std::vector<float> xyzwOfAngles(const ClipTrackData& track, const Vector3& angles){
        if (!isJointAngleTrack(track)) return xyzwOf(angles);
        const float values[pose::DOFS_PER_BONE] = {angles.x, angles.y, angles.z};
        std::vector<float> out(4);
        quaternionToXYZW(pose::jointRotation(pose::boneAt(track.humanBone), values).normalized().toQuaternion(), out.data());
        return out;
    }

    // the names of a track's three components: "x" "y" "z", a slot's DOF names, "left" "up" "forward"
    inline std::vector<std::string> angleComponentNames(const ClipTrackData& track){
        if (isJointAngleTrack(track)){
            const pose::BoneDofInfo& info = pose::boneDofInfo(pose::boneAt(track.humanBone));
            return {info.dofs[0].name, info.dofs[1].name, info.dofs[2].name};
        }
        if (track.channel == ClipChannel::HumanoidPosition) return {"left", "up", "forward"};
        return {"x", "y", "z"};
    }

    inline std::vector<float> flatten(const anim::TrackSample& sample){
        switch (sample.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
            case ClipChannel::HumanoidPosition:
                return {sample.vector.x, sample.vector.y, sample.vector.z};
            case ClipChannel::Rotation:
            case ClipChannel::Humanoid: {
                std::vector<float> out(4);
                quaternionToXYZW(sample.rotation, out.data());
                return out;
            }
            case ClipChannel::Morph:
            case ClipChannel::Property:
                return sample.weights;
            case ClipChannel::Lattice: {
                std::vector<float> out;
                for (const Vector3& v : sample.offsets) out.insert(out.end(), {v.x, v.y, v.z});
                return out;
            }
        }
        return {};
    }

    // a key at `time` on `track` (a copy of `key`'s value), replacing one within the epsilon
    // (`replaced` set); keys stay sorted. Returns the index.
    inline size_t upsertKey(ClipTrackData& track, float time, const ClipKey& given, bool* replaced = nullptr){
        if (replaced) *replaced = false;
        // a rotation key (with its tangents) on the side of the key it replaces or follows: a
        // pasted or added key never makes a Hermite segment flip
        ClipKey key = given;
        if (isRotationChannel(track.channel) && key.value.size() == 4){
            const std::vector<float> aligned = clipui::alignedRotationKeyValue(track, time, key.value, kKeyEpsilon);
            if (aligned != key.value){
                key.value = aligned;
                for (float& v : key.inTangent) v = -v;
                for (float& v : key.outTangent) v = -v;
            }
        }
        auto it = std::lower_bound(track.keys.begin(), track.keys.end(), time - kKeyEpsilon,
                                   [](const ClipKey& k, float t){ return k.time < t; });
        if (it != track.keys.end() && std::fabs(it->time - time) <= kKeyEpsilon){
            *it = key;
            it->time = time;
            if (replaced) *replaced = true;
            return (size_t)(it - track.keys.begin());
        }
        ClipKey inserted = key;
        inserted.time = time;
        it = std::upper_bound(track.keys.begin(), track.keys.end(), time, [](float t, const ClipKey& k){ return t < k.time; });
        const size_t index = (size_t)(it - track.keys.begin());
        track.keys.insert(it, inserted);
        return index;
    }

}
