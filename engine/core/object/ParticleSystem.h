// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SYSTEM_H
#define PARTICLE_SYSTEM_H

// A GameObject with a Particle System (docs/specs/S20-particles.md R14, R31; Unity's
// ParticleSystem): it plays its effect (a .particles asset or an inline one), renders itself
// and raises collision, death and sub-emitter events. The old setters (one initializer and one
// from-to modifier per property) are kept as deprecated shims: each reads the effect's old view,
// changes it and writes it back (LegacyParticleConverter), which keeps the live particles.

#include "object/GameObject.h"
#include "component/ParticleSystemComponent.h"
#include "particles/LegacyParticleSystem.h"
#include "particles/EmitterRuntime.h"

#include <string>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API ParticleSystem: public GameObject{
    private:
        // the deprecated setters' edit of the effect's old view: written back when it ends
        class LegacyEdit;
        LegacyParticleSystem legacyView() const;
        ParticleSystemComponent& system() const;

    public:
        ParticleSystem(Scene* scene);
        // a GameObject with a Particle System; for an old controller (an action entity whose
        // target has the system) the target, with a one-time deprecation note
        ParticleSystem(Scene* scene, Entity entity);
        virtual ~ParticleSystem();

        ParticleSystem(const ParticleSystem& rhs);
        ParticleSystem& operator=(const ParticleSystem& rhs);
        ParticleSystem(ParticleSystem&& rhs) noexcept;
        ParticleSystem& operator=(ParticleSystem&& rhs) noexcept;

        // component checkbox (Unity Behaviour.enabled)
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // ---- playback
        void play();
        void pause();
        // stops emitting; clear also removes the particles
        void stop(bool clear = false);
        void restart();
        void emit(unsigned int count);
        void emit(unsigned int count, const particles::EmitOverrides& overrides);
        void seek(float time);
        void step(unsigned int ticks);
        void sendTrigger(const std::string& name);

        bool isPlaying() const;
        bool isPaused() const;
        bool isEmitting() const;
        unsigned int getParticleCount() const;
        float getTime() const;

        void setPlayOnAwake(bool playOnAwake);
        bool isPlayOnAwake() const;
        void setPlaybackSpeed(float playbackSpeed);
        float getPlaybackSpeed() const;
        // 0: a new random seed at every play
        void setSeed(unsigned int seed);
        unsigned int getSeed() const;

        // the effect: a .particles asset (path), or empty for the inline one
        void setEffect(const std::string& path);
        std::string getEffect() const;
        // the inline effect as JSON (the .particles schema); false (and the errors logged)
        // when it does not read
        bool setInlineEffect(const std::string& json);
        std::string getInlineEffect() const;

        // ---- exposed parameters (the effect's `exposed` list): false for a name it does not expose
        bool setFloat(const std::string& name, float value);
        bool setVector3(const std::string& name, Vector3 value);
        bool setColor(const std::string& name, Vector4 value);
        float getFloat(const std::string& name) const;
        Vector3 getVector3(const std::string& name) const;
        Vector4 getColor(const std::string& name) const;

        // collision planes: GameObjects whose up axis and position are planes (Collision > Planes)
        void addCollisionPlane(Entity plane);
        void clearCollisionPlanes();

        // ---- deprecated (the old Action and its setters)
        void start();
        void reset();
        bool isRunning() const;
        void setTarget(GameObject* target);
        void setTarget(Entity target);
        // the rest of the old Action's members an old script calls (each warns once): the
        // target is where the system is now (its own GameObject), the speed its playback speed,
        // the time count its time; stopped = neither playing nor paused
        Entity getTarget() const;
        void setSpeed(float speed);
        float getSpeed() const;
        float getTimeCount() const;
        bool isStopped() const;

        void setMaxParticles(unsigned int maxParticles);
        unsigned int getMaxParticles() const;

        void setRate(int rate);
        int getRate() const;

        void setEmitter(bool emitter);
        bool isEmitter() const;

        void setLoop(bool loop);
        bool isLoop() const;

        void setLocalSpace(bool localSpace);
        bool isLocalSpace() const;

        void setMaxPerUpdate(int maxPerUpdate);
        int getMaxPerUpdate() const;

        void setLifeInitializer(float life);
        void setLifeInitializer(float minLife, float maxLife);

        void setPositionInitializer(Vector3 position);
        void setPositionInitializer(Vector3 minPosition, Vector3 maxPosition);
        void setPositionInitializerShape(ParticleEmitterShape shape);
        ParticleEmitterShape getPositionInitializerShape() const;
        void setSpherePositionInitializer(float radius);
        void setSpherePositionInitializer(float radius, float innerRadius);
        void setHemispherePositionInitializer(float radius);
        void setHemispherePositionInitializer(float radius, float innerRadius);
        void setCirclePositionInitializer(float radius);
        void setCirclePositionInitializer(float radius, float innerRadius);
        void setConePositionInitializer(float angle, float height);
        void setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition);
        void setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, EaseType functionType);
        void setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, Ease function);

        void addBurst(float time, int count);
        void addBurst(float time, int minCount, int maxCount);
        void setBursts(std::vector<ParticleBurst> bursts);
        void clearBursts();

        void setVelocityInitializer(Vector3 velocity);
        void setVelocityInitializer(Vector3 minVelocity, Vector3 maxVelocity);
        void setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity);
        void setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity, EaseType functionType);
        void setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity, Ease function);

        void setAccelerationInitializer(Vector3 acceleration);
        void setAccelerationInitializer(Vector3 minAcceleration, Vector3 maxAcceleration);
        void setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration);
        void setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration, EaseType functionType);
        void setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration, Ease function);

        void setColorInitializer(Vector3 color);
        void setColorInitializer(Vector3 minColor, Vector3 maxColor);
        void setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor);
        void setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor, EaseType functionType);
        void setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor, Ease function);

        void addColorGradientStop(float time, Vector3 color);
        void setColorGradient(std::vector<ParticleColorGradientStop> stops);
        void clearColorGradient();
        void setColorGradientUseSRGB(bool useSRGB);

        void setAlphaInitializer(float alpha);
        void setAlphaInitializer(float minAlpha, float maxAlpha);
        void setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha);
        void setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha, EaseType functionType);
        void setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha, Ease function);

        void setSizeInitializer(float size);
        void setSizeInitializer(float minSize, float maxSize);
        void setSizeModifier(float fromTime, float toTime, float fromSize, float toSize);
        void setSizeModifier(float fromTime, float toTime, float fromSize, float toSize, EaseType functionType);
        void setSizeModifier(float fromTime, float toTime, float fromSize, float toSize, Ease function);

        void setSpriteIntializer(std::vector<int> frames);
        void setSpriteIntializer(int minFrame, int maxFrame);
        void setSpriteModifier(float fromTime, float toTime, std::vector<int> frames);
        void setSpriteModifier(float fromTime, float toTime, std::vector<int> frames, EaseType functionType);
        void setSpriteModifier(float fromTime, float toTime, std::vector<int> frames, Ease function);

        void setRotationInitializer(Quaternion rotation);
        void setRotationInitializer(float rotation);
        void setRotationInitializer(Quaternion minRotation, Quaternion maxRotation);
        void setRotationInitializer(float minRotation, float maxRotation);
        void setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation);
        void setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation);
        void setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation, EaseType functionType);
        void setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation, EaseType functionType);
        void setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation, Ease function);
        void setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation, Ease function);

        void setScaleInitializer(float scale);
        void setScaleInitializer(Vector3 scale);
        void setScaleInitializer(float minScale, float maxScale);
        void setScaleInitializer(Vector3 minScale, Vector3 maxScale);
        void setScaleModifier(float fromTime, float toTime, float fromScale, float toScale);
        void setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale);
        void setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale, EaseType functionType);
        void setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale, Ease function);
    };
}

#endif //PARTICLE_SYSTEM_H
