// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SYSTEM_COMPONENT_H
#define PARTICLE_SYSTEM_COMPONENT_H

// A particle system (docs/specs/S20-particles.md R1, R14, D1): it lives on a GameObject with a
// Transform and renders itself. Its effect is a .particles asset, or an inline effect (the
// same schema) for migrated and one-off emitters. ParticleSubsystem simulates it and
// RenderSystem draws it; the runtime state is theirs (not saved, not copied).

#include "Engine.h"
#include "asset/AssetRef.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "particles/ParticleEffect.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace particles { struct ParticleSystemState; }

    // a value for one of the effect's exposed parameters (setFloat("Intensity", 2)); Float uses x,
    // Vector3 xyz, Color rgba (sRGB, as edited)
    struct ParticleExposedValue{
        std::string name;
        Vector4 value = Vector4(0.0f, 0.0f, 0.0f, 0.0f);

        bool operator==(const ParticleExposedValue& other) const{
            return name == other.name && value == other.value;
        }
        bool operator!=(const ParticleExposedValue& other) const{ return !(*this == other); }
    };

    // what onCollision, onParticleDeath and onSubEmit listeners get, batched per frame (world space)
    struct ParticleCollisionInfo{
        Vector3 position;
        Vector3 normal;
        Vector3 velocity;
        uint32_t particleId = 0;
        bool killed = false;
    };

    struct ParticleDeathInfo{
        Vector3 position;
        Vector3 velocity;
        uint32_t particleId = 0;
    };

    struct ParticleSubEmitInfo{
        Vector3 position;
        int entry = 0;            // the sub-emitter entry (0-based)
        int count = 0;            // particles it asked its child for
        std::string trigger;      // "birth", "death", "collision" or "trigger"
    };

    struct JASPERENGINE_API ParticleSystemComponent{
        // component checkbox: a disabled system neither simulates nor draws
        bool enabled = true;

        // the effect: a .particles asset; empty: inlineEffect
        AssetRef effect;
        particles::ParticleEffectDocument inlineEffect;

        bool playOnAwake = true;
        // 0: a new random seed at every play (Unity's Auto Random Seed)
        uint32_t seed = 0;
        float playbackSpeed = 1.0f;
        // this system ticks with the unscaled frame time (Engine.timeScale 0 does not stop it:
        // a pause menu, UI particles), as does the effect's own Unscaled Time; the GameObject's
        // own switch, so a prefab with a shared .particles asset plays unscaled without a copy
        bool useUnscaledTime = false;
        // values for the effect's exposed parameters, over the effect's own
        std::vector<ParticleExposedValue> exposedValues;
        // GameObjects whose transforms are more collision planes (their up axis is the
        // normal), with the Collision module's Planes type
        std::vector<Entity> collisionPlanes;

        // runtime (ParticleSubsystem's): the simulation, the effect it runs, the render data
        std::shared_ptr<particles::ParticleSystemState> state;
        // the effect (asset, inline document, exposed values) changed: rebound next update,
        // keeping the live particles when only parameters changed
        bool needReload = true;

        FunctionSubscribe<void(const std::vector<ParticleCollisionInfo>&)> onCollision;
        FunctionSubscribe<void(const std::vector<ParticleDeathInfo>&)> onParticleDeath;
        FunctionSubscribe<void(const std::vector<ParticleSubEmitInfo>&)> onSubEmit;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onCollision); f(onParticleDeath); f(onSubEmit); }
    };

}

#endif //PARTICLE_SYSTEM_COMPONENT_H
