// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MorphTracks.h"

#include "component/KeyframeTracksComponent.h"
#include "component/MorphTracksComponent.h"

using namespace jasperengine;

MorphTracks::MorphTracks(Scene* scene): Action(scene){
    addComponent<KeyframeTracksComponent>();
    addComponent<MorphTracksComponent>();
}

MorphTracks::MorphTracks(Scene* scene, Entity entity): Action(scene, entity){

}

MorphTracks::MorphTracks(Scene* scene, std::vector<float> times, std::vector<std::vector<float>> values): MorphTracks(scene){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    MorphTracksComponent& morphtracks = getComponent<MorphTracksComponent>();

    morphtracks.values = values;
}

void MorphTracks::setTimes(std::vector<float> times){
    getComponent<KeyframeTracksComponent>().setTimes(times);
}

void MorphTracks::setValues(std::vector<std::vector<float>> values){
    MorphTracksComponent& morphtracks = getComponent<MorphTracksComponent>();

    morphtracks.values = values;

    // keep Hermite tangents (GLTF CUBICSPLINE) mirrored with values
    std::vector<float> zeroTangent(values.empty() ? 0 : values.back().size(), 0.0f);
    if (!morphtracks.inTangents.empty()){
        morphtracks.inTangents.resize(values.size(), zeroTangent);
    }
    if (!morphtracks.outTangents.empty()){
        morphtracks.outTangents.resize(values.size(), zeroTangent);
    }
}

void MorphTracks::setEasings(std::vector<EaseType> easings){
    getComponent<KeyframeTracksComponent>().setEasings(easings);
}

void MorphTracks::setEasing(unsigned int segment, EaseType ease){
    getComponent<KeyframeTracksComponent>().setEasing(segment, ease);
}
