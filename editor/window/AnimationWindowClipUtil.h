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

    inline std::string rowKey(const ClipTrackData& track){
        return track.path + "|" + anim::channelName(track.channel);
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

    inline std::vector<float> flatten(const anim::TrackSample& sample){
        switch (sample.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
                return {sample.vector.x, sample.vector.y, sample.vector.z};
            case ClipChannel::Rotation: {
                std::vector<float> out(4);
                quaternionToXYZW(sample.rotation, out.data());
                return out;
            }
            case ClipChannel::Morph:
                return sample.weights;
            case ClipChannel::Lattice: {
                std::vector<float> out;
                for (const Vector3& v : sample.offsets) out.insert(out.end(), {v.x, v.y, v.z});
                return out;
            }
            case ClipChannel::Property:
                break;
        }
        return {};
    }

    // a key at `time` on `track` (a copy of `key`'s value), replacing one within the epsilon
    // (`replaced` set); keys stay sorted. Returns the index.
    inline size_t upsertKey(ClipTrackData& track, float time, const ClipKey& key, bool* replaced = nullptr){
        if (replaced) *replaced = false;
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

    // a time label: the frame number, or seconds
    inline std::string formatTime(float time, float fps, bool frames){
        char text[48];
        if (frames){
            std::snprintf(text, sizeof(text), "%d", (int)std::lround(time * fps));
        }else{
            std::snprintf(text, sizeof(text), "%.2f", time);
        }
        return text;
    }

    // both forms, the shown one first: "frame 21 (0.700 s)" / "0.700 s (frame 21)"
    inline std::string formatBoth(float time, float fps, bool frames){
        char text[64];
        const long frame = std::lround(time * fps);
        if (frames) std::snprintf(text, sizeof(text), "frame %ld (%.3f s)", frame, time);
        else std::snprintf(text, sizeof(text), "%.3f s (frame %ld)", time, frame);
        return text;
    }

}
