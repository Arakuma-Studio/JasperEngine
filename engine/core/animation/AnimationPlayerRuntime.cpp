// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayerRuntime.h"

using namespace jasperengine;

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
}
