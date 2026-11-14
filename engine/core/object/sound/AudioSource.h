// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioSource_h
#define AudioSource_h

#include "EntityHandle.h"
#include "GameObject.h"
#include "component/AudioSourceComponent.h"
#include "object/sound/AudioSourceEffect.h"

namespace jasperengine {

    class JASPERENGINE_API AudioSource: public EntityHandle{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Sound is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        // a new GameObject (a Transform) with an Audio Source: 2D (Spatial Blend 0), or 3D (1)
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
        // Unity's AudioClip.loadState of the source's clip: Loading from loadSound until ready
        // (getLength() is 0 until Loaded) or Failed
        AudioLoadState getLoadState() const;

        // an entity without a Transform (an older AudioSource(scene)) gets one and stays 2D
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

        // true: a Transform (added when missing) and Spatial Blend 1; false: Spatial Blend 0
        void setSound3D(bool sound3D);
        // a Transform and Spatial Blend above 0
        bool isSound3D() const;

        // 0 = 2D (pan, no attenuation) .. 1 = 3D; clamped to 0..1 (S97 REQ-18)
        void setSpatialBlend(float spatialBlend);
        float getSpatialBlend() const;

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

        // ---- mixer and voices (audio-mixer spec 7.3, 8)

        // the mixer group by name ("" = Master); a name not in the mixer plays through Master
        void setOutputGroup(const std::string& groupName);
        std::string getOutputGroup() const;

        void setLoadType(AudioLoadType loadType);
        AudioLoadType getLoadType() const;
        // what Auto resolves to for the current file (by its size)
        AudioLoadType getResolvedLoadType() const;

        // 0 (most important) .. 256 (least); default 128
        void setPriority(int priority);
        int getPriority() const;

        // main + one-shot voices of this source (1..64, default 8)
        void setMaxVoices(int maxVoices);
        int getMaxVoices() const;

        void setIgnoreListenerPause(bool ignore);
        bool isIgnoreListenerPause() const;

        // ---- Sound Events and triggers (docs/specs/S37-sound-events.md R9, R12)
        // the Sound Event it plays, its path; assigning a path or GUID resolves it ("" clears it)
        void setEvent(const std::string& pathOrGuid);
        std::string getEvent() const;
        // AudioPlayOn flags
        void setPlayOn(uint32_t flags);
        uint32_t getPlayOn() const;
        void setMinImpactSpeed(float speed);
        float getMinImpactSpeed() const;
        void setFullVolumeImpactSpeed(float speed);
        float getFullVolumeImpactSpeed() const;
        // clip mode: each start +- this many semitones (0..24), and -this .. 0 dB (0..60)
        void setPitchRandomSemitones(float semitones);
        float getPitchRandomSemitones() const;
        void setVolumeRandomDb(float db);
        float getVolumeRandomDb() const;

        // Unity's name for speed
        void setPitch(float pitch);
        float getPitch() const;

        // Unity PlayOneShot: an extra voice of the clip, or of a Sound Event's pick (its path or
        // GUID), over what plays (volume x volumeScale)
        void playOneShot(const std::string& filename, float volumeScale = 1.0f);
        // plays from silence to the current volume (starting it when it is not playing)
        void fadeIn(double seconds, bool unscaled = false);
        // to silence, then stops (or pauses); the volume is put back for the next play()
        void fadeOut(double seconds, bool pause = false, bool unscaled = false);
        // the volume moves to `volume` over `seconds`
        void fadeTo(double volume, double seconds, bool unscaled = false);
        bool isFading() const;
        // the voices playing now (main + one-shots)
        int getVoiceCount() const;

        // ---- its own effects (docs/specs/S97-audio-gaps.md REQ-1, REQ-41; AudioSourceEffect.cpp)
        // appends a mixer effect (not duck or limiter), up to AudioSourceComponent::maxEffects; an
        // invalid effect (logged) when it cannot
        AudioSourceEffect addEffect(const std::string& type);
        // by 1-based index, or the first of a type; invalid when there is none
        AudioSourceEffect getEffect(int index) const;
        AudioSourceEffect getEffect(const std::string& type) const;
        bool removeEffect(int index);
        bool removeEffect(const std::string& type);
        int getEffectCount() const;

        // Unity AudioSource.PlayClipAtPoint: a voice of `scene` at a world position, stopped with
        // the scene; through a mixer group by name ("" = Master)
        static void playClipAtPoint(Scene* scene, const std::string& filename, Vector3 position,
                                    float volume = 1.0f, const std::string& group = "");
    };

}

#endif /* AudioSource_h */
