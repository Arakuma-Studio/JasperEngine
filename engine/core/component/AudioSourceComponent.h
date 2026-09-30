// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIO_SOURCE_COMPONENT_H
#define AUDIO_SOURCE_COMPONENT_H

#include "Engine.h"

#include <memory>

namespace SoLoud{
    class Wav;
}

namespace jasperengine{

    enum class AudioSourceState{
        Playing,
        Paused,
        Stopped
    };

    enum class AudioSourceAttenuation{
        NO_ATTENUATION,
        INVERSE_DISTANCE,
        LINEAR_DISTANCE,
        EXPONENTIAL_DISTANCE
    };

    struct JASPERENGINE_API AudioSourceComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        std::shared_ptr<SoLoud::Wav> sample = nullptr;
        unsigned int handle = 0; //Soloud handle

        AudioSourceState state = AudioSourceState::Stopped;

        std::string filename;
        bool loaded = false;
        bool loadFailed = false;

        bool enableClocked = false;
        Vector3 lastPosition = Vector3(0, 0, 0);

        bool startTrigger = false;
        bool stopTrigger = false;
        bool pauseTrigger = false;

        FunctionSubscribe<void()> onStart;
        FunctionSubscribe<void()> onPause;
        FunctionSubscribe<void()> onStop;

        double volume = 1;
        float speed = 1;
        float pan = 0; // -1 is left, 0 is middle and 1 is right
        bool looping = false;
        double loopingPoint = 0;
        bool protectVoice = false;
        bool inaudibleBehaviorMustTick = false;
        bool inaudibleBehaviorKill = false;

        // for 3D sound
        float minDistance = 1.0;
        float maxDistance = 1000.0;
        AudioSourceAttenuation attenuationModel = AudioSourceAttenuation::NO_ATTENUATION;
        float attenuationRolloffFactor = 1.0; // means that end of max distance sound will be zero
        float dopplerFactor = 1.0;

        // setted by system
        double length = 0;
        double playingTime = 0;

        bool needUpdate = true;
    };

}

#endif //AUDIO_SOURCE_COMPONENT_H
