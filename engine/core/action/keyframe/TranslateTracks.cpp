// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TranslateTracks.h"

#include "component/KeyframeTracksComponent.h"
#include "component/TranslateTracksComponent.h"

using namespace jasperengine;

TranslateTracks::TranslateTracks(Scene* scene): KeyframeTracks(scene){
    addComponent<TranslateTracksComponent>();
}

TranslateTracks::TranslateTracks(Scene* scene, Entity entity): KeyframeTracks(scene, entity){

}

TranslateTracks::TranslateTracks(Scene* scene, std::vector<float> times, std::vector<Vector3> values): TranslateTracks(scene){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    TranslateTracksComponent& translatetracks = getComponent<TranslateTracksComponent>();

    translatetracks.values = values;
}

void TranslateTracks::setValues(std::vector<Vector3> values){
    TranslateTracksComponent& translatetracks = getComponent<TranslateTracksComponent>();

    translatetracks.values = values;

    // keep Hermite tangents (GLTF CUBICSPLINE) mirrored with values
    if (!translatetracks.inTangents.empty()){
        translatetracks.inTangents.resize(values.size(), Vector3::ZERO);
    }
    if (!translatetracks.outTangents.empty()){
        translatetracks.outTangents.resize(values.size(), Vector3::ZERO);
    }
}
