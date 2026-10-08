// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioSource_h
#define AudioSource_h

#include "EntityHandle.h"
#include "GameObject.h"
#include "component/AudioSourceComponent.h"

namespace jasperengine {

    class JASPERENGINE_API AudioSource: public EntityHandle{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Sound is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        AudioSource(Scene* scene);
        AudioSource(Scene* scene, bool is3D);
        AudioSource(Scene* scene, Entity entity);
        virtual ~AudioSource();

        AudioSource(const AudioSource& rhs);
        AudioSource& operator=(const AudioSource& rhs);
        AudioSource(AudioSource&& rhs) noexcept;
        AudioSource& operator=(AudioSource&& rhs) noexcept;

        int loadSound(const std::string& filename);
        void destroySound();

        GameObject getGameObject() const;

        void play();
        void pause();
        void stop();
        void seek(double time);

        double getLength();
        double getPlayingTime();

        bool isPlaying();
        bool isPaused();
        bool isStopped();

        void setSound3D(bool sound3D);
        bool isSound3D() const;

        void setClockedSound(bool enableClocked);
        bool isClockedSound() const;

        void setVolume(double volume);
        double getVolume() const;

        void setSpeed(float speed);
        float getSpeed() const;

        void setPan(float pan);
        float getPan() const;

        void setLooping(bool looping);
        bool isLooping() const;

        void setLoopingPoint(double loopingPoint);
        double getLoopingPoint() const;

        void setProtectVoice(bool protectVoice);
        bool isProtectVoice() const;

        void setInaudibleBehavior(bool mustTick, bool kill);

        void setMinMaxDistance(float minDistance, float maxDistance);

        void setMinDistance(float minDistance);
        float getMinDistance() const;

        void setMaxDistance(float maxDistance);
        float getMaxDistance() const;

        void setAttenuationModel(AudioSourceAttenuation attenuationModel);
        AudioSourceAttenuation getAttenuationModel() const;

        void setAttenuationRolloffFactor(float attenuationRolloffFactor);
        float getAttenuationRolloffFactor() const;

        void setDopplerFactor(float dopplerFactor);
        float getDopplerFactor() const;
    };

}

#endif /* AudioSource_h */
