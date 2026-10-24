// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RotateTracks.h"

#include "component/KeyframeTracksComponent.h"
#include "component/RotateTracksComponent.h"

using namespace jasperengine;

RotateTracks::RotateTracks(Scene* scene): KeyframeTracks(scene){
    addComponent<RotateTracksComponent>();
}

RotateTracks::RotateTracks(Scene* scene, Entity entity): KeyframeTracks(scene, entity){

}

RotateTracks::RotateTracks(Scene* scene, std::vector<float> times, std::vector<Quaternion> values): RotateTracks(scene){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    RotateTracksComponent& rotatetracks = getComponent<RotateTracksComponent>();

    rotatetracks.values = values;
}

void RotateTracks::setValues(std::vector<Quaternion> values){
    RotateTracksComponent& rotatetracks = getComponent<RotateTracksComponent>();

    rotatetracks.values = values;

    // keep Hermite tangents (GLTF CUBICSPLINE) mirrored with values
    if (!rotatetracks.inTangents.empty()){
        rotatetracks.inTangents.resize(values.size(), Quaternion(0.0f, 0.0f, 0.0f, 0.0f));
    }
    if (!rotatetracks.outTangents.empty()){
        rotatetracks.outTangents.resize(values.size(), Quaternion(0.0f, 0.0f, 0.0f, 0.0f));
    }
}
