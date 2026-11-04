// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "Audio.h"

#include "object/EntityHandle.h"
#include "subsystem/AudioSystem.h"

using namespace jasperengine;

PlayOptions& PlayOptions::following(const EntityHandle& gameObject) {
    targetScene = gameObject.getScene();
    target = gameObject.getEntity();
    return *this;
}

SoundHandle Audio::play(const std::string& eventOrClip, const PlayOptions& options) {
    return AudioSystem::playFree(eventOrClip, options, "cpp");
}

int Audio::stopAll(const std::string& group) {
    return AudioSystem::stopFreeAndOneShots(group);
}

#endif // JASPERENGINE_SOUND_LAYER
