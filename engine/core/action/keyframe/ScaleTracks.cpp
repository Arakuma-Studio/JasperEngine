// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScaleTracks.h"

#include "component/KeyframeTracksComponent.h"
#include "component/ScaleTracksComponent.h"

using namespace jasperengine;

ScaleTracks::ScaleTracks(Scene* scene): Action(scene){
    addComponent<KeyframeTracksComponent>();
    addComponent<ScaleTracksComponent>();
}

ScaleTracks::ScaleTracks(Scene* scene, Entity entity): Action(scene, entity){

}

ScaleTracks::ScaleTracks(Scene* scene, std::vector<float> times, std::vector<Vector3> values): ScaleTracks(scene){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    ScaleTracksComponent& scaletracks = getComponent<ScaleTracksComponent>();

    scaletracks.values = values;
}

void ScaleTracks::setTimes(std::vector<float> times){
    getComponent<KeyframeTracksComponent>().setTimes(times);
}

void ScaleTracks::setValues(std::vector<Vector3> values){
    ScaleTracksComponent& scaletracks = getComponent<ScaleTracksComponent>();

    scaletracks.values = values;

    // keep Hermite tangents (GLTF CUBICSPLINE) mirrored with values
    if (!scaletracks.inTangents.empty()){
        scaletracks.inTangents.resize(values.size(), Vector3::ZERO);
    }
    if (!scaletracks.outTangents.empty()){
        scaletracks.outTangents.resize(values.size(), Vector3::ZERO);
    }
}

void ScaleTracks::setEasings(std::vector<EaseType> easings){
    getComponent<KeyframeTracksComponent>().setEasings(easings);
}

void ScaleTracks::setEasing(unsigned int segment, EaseType ease){
    getComponent<KeyframeTracksComponent>().setEasing(segment, ease);
}
