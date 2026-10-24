// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "KeyframeTracks.h"

#include "component/KeyframeTracksComponent.h"

using namespace jasperengine;

KeyframeTracks::KeyframeTracks(Scene* scene): Action(scene){
    addComponent<KeyframeTracksComponent>();
}

KeyframeTracks::KeyframeTracks(Scene* scene, Entity entity): Action(scene, entity){

}

void KeyframeTracks::setTimes(std::vector<float> times){
    getComponent<KeyframeTracksComponent>().setTimes(times);
}

void KeyframeTracks::setEasings(std::vector<EaseType> easings){
    getComponent<KeyframeTracksComponent>().setEasings(easings);
}

void KeyframeTracks::setEasing(unsigned int segment, EaseType ease){
    getComponent<KeyframeTracksComponent>().setEasing(segment, ease);
}

void KeyframeTracks::setLoop(bool loop){
    getComponent<KeyframeTracksComponent>().loop = loop;
}

bool KeyframeTracks::isLoop() const{
    return getComponent<KeyframeTracksComponent>().loop;
}

void KeyframeTracks::setRelative(bool relative){
    getComponent<KeyframeTracksComponent>().relative = relative;
}

bool KeyframeTracks::isRelative() const{
    return getComponent<KeyframeTracksComponent>().relative;
}
