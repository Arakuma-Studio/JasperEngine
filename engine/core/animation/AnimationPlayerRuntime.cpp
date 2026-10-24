// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayerRuntime.h"

#include "animation/ModelSubClips.h"

#include <algorithm>

using namespace jasperengine;

std::string anim::defaultStateName(const AnimationPlayerClip& entry){
    if (!entry.name.empty()) return entry.name;
    SubClipReference reference;
    if (parseSubClipReference(entry.clip, reference)) return reference.clip;
    size_t slash = entry.clip.find_last_of("/\\");
    std::string file = slash == std::string::npos ? entry.clip : entry.clip.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
}

std::vector<std::string> anim::playerStateNames(const std::vector<AnimationPlayerClip>& clips){
    std::vector<std::string> names;
    names.reserve(clips.size());
    for (const AnimationPlayerClip& entry : clips){
        std::string base = defaultStateName(entry);
        std::string name = base;
        for (int k = 2; std::find(names.begin(), names.end(), name) != names.end(); k++){
            name = base + " " + std::to_string(k);
        }
        names.push_back(name);
    }
    return names;
}

int anim::AnimationPlayerRuntime::findState(const std::string& name) const{
    for (size_t i = 0; i < states.size(); i++){
        if (states[i].name == name) return (int)i;
    }
    return -1;
}

void anim::ClipPreviewSession::setRest(Entity entity, const ChannelRest& value){
    rest[entity] = value;
}

void anim::ClipPreviewSession::reset(){
    root = NULL_ENTITY;
    rest.clear();
    bindings.clear();
    written.clear();
    previousTimes.clear();
}
