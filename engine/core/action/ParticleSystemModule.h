// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SYSTEM_MODULE_H
#define PARTICLE_SYSTEM_MODULE_H

// A live view of one module of a Particle System (Lua's ps.main, ps.emission, ... ps.renderer;
// particles spec 6.6): it holds the GameObject, not the values, so `local e = ps.emission;
// e.rateOverTime = 0` changes the system as Unity's module structs do. Its fields are those of
// particles/ParticleFields (script/binding/ParticleClassesLua.cpp reads and writes them).

#include "Export.h"
#include "ecs/Entity.h"

namespace jasperengine{

    class Scene;
    struct ParticleSystemComponent;

    struct JASPERENGINE_API ParticleSystemModule{
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        // its index in particles::moduleNames()
        int module = 0;

        // null when the GameObject or its Particle System is gone
        ParticleSystemComponent* component() const;
        const char* moduleName() const;
    };

}

#endif //PARTICLE_SYSTEM_MODULE_H
