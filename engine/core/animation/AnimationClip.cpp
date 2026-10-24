// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClip.h"

#include <algorithm>

using namespace jasperengine;

// ClipData helpers (complete: pure functions of the value form)

float ClipData::effectiveLength() const{
    if (hasExplicitLength()) return length;
    float last = 0.0f;
    for (const ClipTrackData& track : tracks){
        if (track.channel == ClipChannel::Property) continue;
        last = std::max(last, track.lastKeyTime());
    }
    for (const ClipEventData& event : events){
        last = std::max(last, event.time);
    }
    return last;
}

int ClipData::findTrack(const std::string& path, ClipChannel channel) const{
    if (channel == ClipChannel::Property) return -1;
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].path == path) return (int)i;
    }
    return -1;
}

size_t ClipData::valueWidth(ClipChannel channel, size_t morphCount, size_t pointCount){
    switch (channel){
        case ClipChannel::Position:
        case ClipChannel::Scale:
            return 3;
        case ClipChannel::Rotation:
            return 4;
        case ClipChannel::Morph:
            return morphCount;
        case ClipChannel::Lattice:
            return 3 * pointCount;
        case ClipChannel::Property:
            return 0;
    }
    return 0;
}

// AnimationClip

AnimationClip::~AnimationClip(){
}

std::shared_ptr<const AnimationClip> AnimationClip::build(const ClipData& data, std::vector<ClipWarning>* warnings){
    // TODO(T1a): normalize `data`, fill the contiguous storage and the Track pointers
    (void)data;
    (void)warnings;
    return std::shared_ptr<const AnimationClip>(new AnimationClip());
}

ClipData AnimationClip::toData() const{
    // TODO(T1a): the value form of this clip
    return ClipData();
}

int AnimationClip::findTrack(const std::string& path, ClipChannel channel) const{
    if (channel == ClipChannel::Property) return -1;
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].path == path) return (int)i;
    }
    return -1;
}
