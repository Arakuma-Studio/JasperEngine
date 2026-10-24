// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeTracks.h"

#include "component/KeyframeTracksComponent.h"
#include "component/LatticeTracksComponent.h"

using namespace jasperengine;

LatticeTracks::LatticeTracks(Scene* scene): Action(scene){
    addComponent<KeyframeTracksComponent>();
    addComponent<LatticeTracksComponent>();
}

LatticeTracks::LatticeTracks(Scene* scene, Entity entity): Action(scene, entity){

}

LatticeTracks::LatticeTracks(Scene* scene, std::vector<float> times, std::vector<int> points, std::vector<std::vector<Vector3>> values): LatticeTracks(scene){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    LatticeTracksComponent& latticetracks = getComponent<LatticeTracksComponent>();

    latticetracks.points = points;
    latticetracks.values = values;
}

void LatticeTracks::setTimes(std::vector<float> times){
    getComponent<KeyframeTracksComponent>().setTimes(times);
}

void LatticeTracks::setPoints(std::vector<int> points){
    getComponent<LatticeTracksComponent>().points = points;
}

void LatticeTracks::setValues(std::vector<std::vector<Vector3>> values){
    LatticeTracksComponent& latticetracks = getComponent<LatticeTracksComponent>();

    latticetracks.values = values;

    // keep Hermite tangents (GLTF CUBICSPLINE style) mirrored with values
    std::vector<Vector3> zeroTangent(values.empty() ? 0 : values.back().size(), Vector3(0, 0, 0));
    if (!latticetracks.inTangents.empty()){
        latticetracks.inTangents.resize(values.size(), zeroTangent);
    }
    if (!latticetracks.outTangents.empty()){
        latticetracks.outTangents.resize(values.size(), zeroTangent);
    }
}

void LatticeTracks::setEasings(std::vector<EaseType> easings){
    getComponent<KeyframeTracksComponent>().setEasings(easings);
}

void LatticeTracks::setEasing(unsigned int segment, EaseType ease){
    getComponent<KeyframeTracksComponent>().setEasing(segment, ease);
}
