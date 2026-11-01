// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipTangents.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // the linear slopes of component c into and out of key k
    float slopeBefore(const ClipTrackData& track, size_t k, size_t c){
        if (k == 0) return 0.0f;
        const ClipKey& a = track.keys[k - 1];
        const ClipKey& b = track.keys[k];
        const float dt = b.time - a.time;
        if (!(dt > 0.0f) || c >= a.value.size() || c >= b.value.size()) return 0.0f;
        return (b.value[c] - a.value[c]) / dt;
    }

    float slopeAfter(const ClipTrackData& track, size_t k, size_t c){
        if (k + 1 >= track.keys.size()) return 0.0f;
        const ClipKey& a = track.keys[k];
        const ClipKey& b = track.keys[k + 1];
        const float dt = b.time - a.time;
        if (!(dt > 0.0f) || c >= a.value.size() || c >= b.value.size()) return 0.0f;
        return (b.value[c] - a.value[c]) / dt;
    }

    void linearTangents(const ClipTrackData& track, size_t k, ClipKey& key){
        const size_t width = key.value.size();
        key.inTangent.assign(width, 0.0f);
        key.outTangent.assign(width, 0.0f);
        const bool first = k == 0;
        const bool last = k + 1 >= track.keys.size();
        for (size_t c = 0; c < width; c++){
            const float before = slopeBefore(track, k, c);
            const float after = slopeAfter(track, k, c);
            key.inTangent[c] = first ? after : before;
            key.outTangent[c] = last ? before : after;
        }
    }

    void autoTangents(const ClipTrackData& track, size_t k, ClipKey& key){
        const size_t width = key.value.size();
        key.inTangent.assign(width, 0.0f);
        key.outTangent.assign(width, 0.0f);
        const bool hasPrevious = k > 0;
        const bool hasNext = k + 1 < track.keys.size();
        for (size_t c = 0; c < width; c++){
            const float pt = hasPrevious ? track.keys[k - 1].time : 0.0f;
            const float pv = hasPrevious && c < track.keys[k - 1].value.size() ? track.keys[k - 1].value[c] : 0.0f;
            const float nt = hasNext ? track.keys[k + 1].time : 0.0f;
            const float nv = hasNext && c < track.keys[k + 1].value.size() ? track.keys[k + 1].value[c] : 0.0f;
            const float slope = cliptangents::clampedAutoSlope(pt, pv, key.time, key.value[c], nt, nv, hasPrevious, hasNext);
            key.inTangent[c] = slope;
            key.outTangent[c] = slope;
        }
    }

    // a key's step ease (Constant right) goes when that side becomes free or linear
    void unstepRight(ClipTrackData& track, size_t k){
        if (track.keys[k].ease == EaseType::STEP) track.keys[k].ease = EaseType::LINEAR;
    }
    void unstepLeft(ClipTrackData& track, size_t k){
        if (k > 0 && track.keys[k - 1].ease == EaseType::STEP) track.keys[k - 1].ease = EaseType::LINEAR;
    }

}

bool cliptangents::editable(const ClipTrackData& track){
    if (track.channel == ClipChannel::Rotation) return false;
    if (track.channel == ClipChannel::Property) return track.known && !track.discrete;
    return true;
}

float cliptangents::clampedAutoSlope(float previousTime, float previousValue, float time, float value, float nextTime, float nextValue,
                                     bool hasPrevious, bool hasNext){
    // the ends are flat (Unity: a two-key Auto curve eases in and out)
    if (!hasPrevious || !hasNext) return 0.0f;
    const float dtPrevious = time - previousTime;
    const float dtNext = nextTime - time;
    if (!(dtPrevious > 0.0f) || !(dtNext > 0.0f)) return 0.0f;
    const float dPrevious = (value - previousValue) / dtPrevious;
    const float dNext = (nextValue - value) / dtNext;
    // a local extremum (or a flat side): flat, no overshoot
    if (dPrevious * dNext <= 0.0f) return 0.0f;
    float slope = (nextValue - previousValue) / (nextTime - previousTime);
    const float limit = 3.0f * std::min(std::fabs(dPrevious), std::fabs(dNext));
    if (std::fabs(slope) > limit) slope = slope < 0.0f ? -limit : limit;
    return slope;
}

void cliptangents::ensureTangents(ClipTrackData& track){
    bool all = true;
    for (const ClipKey& key : track.keys) all = all && key.hasTangents() && key.inTangent.size() == key.value.size();
    if (all) return;
    // the keys without get the straight line to their neighbours (the stored ones stay)
    std::vector<ClipKey> keys = track.keys;
    for (size_t k = 0; k < keys.size(); k++){
        if (keys[k].hasTangents() && keys[k].inTangent.size() == keys[k].value.size()) continue;
        linearTangents(track, k, keys[k]);
        if (keys[k].tangentMode == ClipTangentMode::None) keys[k].tangentMode = ClipTangentMode::Linear;
    }
    for (ClipKey& key : keys){
        // tangents win over the eases (a step stays: Constant)
        if (key.ease != EaseType::STEP) key.ease = EaseType::LINEAR;
    }
    track.keys = std::move(keys);
}

size_t cliptangents::droppedEases(const ClipTrackData& track){
    bool all = true;
    for (const ClipKey& key : track.keys) all = all && key.hasTangents() && key.inTangent.size() == key.value.size();
    if (all) return 0;
    size_t count = 0;
    for (const ClipKey& key : track.keys){
        if (key.ease != EaseType::LINEAR && key.ease != EaseType::STEP) count++;
    }
    return count;
}

void cliptangents::refresh(ClipTrackData& track){
    if (!editable(track)) return;
    bool any = false;
    for (const ClipKey& key : track.keys){
        any = any || key.tangentMode == ClipTangentMode::Auto || key.tangentMode == ClipTangentMode::Linear ||
              key.tangentMode == ClipTangentMode::Flat;
    }
    if (!any) return;
    ensureTangents(track);
    for (size_t k = 0; k < track.keys.size(); k++){
        ClipKey& key = track.keys[k];
        switch (key.tangentMode){
            case ClipTangentMode::Auto: autoTangents(track, k, key); break;
            case ClipTangentMode::Linear: linearTangents(track, k, key); break;
            case ClipTangentMode::Flat:
                key.inTangent.assign(key.value.size(), 0.0f);
                key.outTangent.assign(key.value.size(), 0.0f);
                break;
            default: break;
        }
    }
}

void cliptangents::setMode(ClipTrackData& track, size_t k, ClipTangentMode mode){
    if (!editable(track) || k >= track.keys.size()) return;
    ensureTangents(track);
    ClipKey& key = track.keys[k];
    key.tangentMode = mode;
    if (mode != ClipTangentMode::Broken){
        // a smooth mode has no constant sides
        unstepRight(track, k);
        unstepLeft(track, k);
    }
    switch (mode){
        case ClipTangentMode::Auto: autoTangents(track, k, key); break;
        case ClipTangentMode::Linear: linearTangents(track, k, key); break;
        case ClipTangentMode::Flat:
            key.inTangent.assign(key.value.size(), 0.0f);
            key.outTangent.assign(key.value.size(), 0.0f);
            break;
        case ClipTangentMode::Free:
            // Free Smooth: one slope through the key (the two sides' mean)
            for (size_t c = 0; c < key.value.size(); c++){
                const float mean = 0.5f * (key.inTangent[c] + key.outTangent[c]);
                key.inTangent[c] = mean;
                key.outTangent[c] = mean;
            }
            break;
        default: break;
    }
}

void cliptangents::setSide(ClipTrackData& track, size_t k, Side side, SideType type){
    if (!editable(track) || k >= track.keys.size()) return;
    ensureTangents(track);
    ClipKey& key = track.keys[k];
    const bool left = side != Side::Right;
    const bool right = side != Side::Left;
    for (size_t c = 0; c < key.value.size(); c++){
        if (left && type == SideType::Linear) key.inTangent[c] = k > 0 ? slopeBefore(track, k, c) : slopeAfter(track, k, c);
        if (right && type == SideType::Linear) key.outTangent[c] = k + 1 < track.keys.size() ? slopeAfter(track, k, c) : slopeBefore(track, k, c);
    }
    if (left){
        if (type == SideType::Constant){
            if (k > 0) track.keys[k - 1].ease = EaseType::STEP;
        }else{
            unstepLeft(track, k);
        }
    }
    if (right){
        if (type == SideType::Constant) key.ease = EaseType::STEP;
        else unstepRight(track, k);
    }
    key.tangentMode = (side == Side::Both && type == SideType::Linear) ? ClipTangentMode::Linear : ClipTangentMode::Broken;
}

void cliptangents::setSlope(ClipTrackData& track, size_t k, size_t component, Side side, float slope){
    if (!editable(track) || k >= track.keys.size() || !std::isfinite(slope)) return;
    ensureTangents(track);
    ClipKey& key = track.keys[k];
    if (component >= key.value.size()) return;
    const bool broken = key.tangentMode == ClipTangentMode::Broken;
    if (side != Side::Right || !broken){
        key.inTangent[component] = slope;
        unstepLeft(track, k);
    }
    if (side != Side::Left || !broken){
        key.outTangent[component] = slope;
        unstepRight(track, k);
    }
    if (!broken) key.tangentMode = ClipTangentMode::Free;
}
