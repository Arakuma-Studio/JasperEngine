// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SYSTEM_H
#define PARTICLE_SYSTEM_H

#include "Action.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleUpgrade.h"

namespace jasperengine{

    class JASPERENGINE_API ParticleSystem: public Action{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Particles is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        // a version 1 (legacy) system on a new GameObject, as before the module model
        ParticleSystem(Scene* scene);
        // modules: a version 2 system drawing on its own new GameObject (a Transform, the
        // default particle quad and its instanced mesh); false is the constructor above
        ParticleSystem(Scene* scene, bool modules);
        ParticleSystem(Scene* scene, Entity entity);
        virtual ~ParticleSystem();

        ParticleSystem(const ParticleSystem& rhs);
        ParticleSystem& operator=(const ParticleSystem& rhs);
        ParticleSystem(ParticleSystem&& rhs) noexcept;
        ParticleSystem& operator=(ParticleSystem&& rhs) noexcept;

        void reset();

        // ---- version 2 (particles spec 6.6): Unity's ParticleSystem control; withChildren
        // also drives every version 2 system of the GameObject's children. A version 1 system
        // keeps the Action behaviour (start / stop / pause)
        void play(bool withChildren = true);
        void stop(bool withChildren = true, ParticleSystemStopBehavior behavior = ParticleSystemStopBehavior::StopEmitting);
        void pause(bool withChildren = true);
        void clear(bool withChildren = true);
        // `count` particles now (also while stopped); the number spawned
        int emit(int count);
        void simulate(float time, bool withChildren = true, bool restart = true);
        bool isPlaying() const;
        bool isPaused() const;
        bool isStopped() const;
        bool isEmitting() const;
        bool isAlive(bool withChildren = true) const;
        unsigned int getParticleCount() const;
        // the system clock (0 .. main.duration)
        float getTime() const;
        void setTime(float time);
        uint32_t getRandomSeed() const;
        void setRandomSeed(uint32_t seed);
        bool getUseAutoRandomSeed() const;
        void setUseAutoRandomSeed(bool automatic);
        bool getUseUnscaledTime() const;
        void setUseUnscaledTime(bool unscaled);
        int getVersion() const;
        // 1 -> 2 runs upgrade(); 2 -> 1 is refused (an error in the log)
        void setVersion(int version);
        // the legacy model as modules (particles/ParticleUpgrade): the component becomes version 2
        // unless something would be dropped (a position modifier) and `force` is off
        particles::UpgradeReport upgrade(bool force = false);

        // The default renderer of a version 2 system on its own GameObject: a Transform when it
        // has none, a 1 x 1 quad facing +Z drawn with library:unlit (the built-in soft particle
        // texture, alpha blended, no depth write, both faces) and an empty InstancedMeshComponent
        // the system fills; the action targets the GameObject itself. Used by the editor (the
        // menu, create_particles, the Unity importer) and by ParticleSystem(scene, true)
        static void setupDefaultRenderer(Scene* scene, Entity entity);
        // the target mesh of a Mesh render mode: "quad", "cube", "sphere", "cylinder", "capsule",
        // "plane" (a model path is the editor's: false)
        static bool applyRendererMesh(Scene* scene, Entity entity, const std::string& mesh);

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