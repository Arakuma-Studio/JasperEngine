// Minimal owner for the real scheduler; excludes scene systems and graphics startup.
#pragma once

#include "ecs/ComponentManager.h"

namespace jasperengine {

class EntityRegistry {};

class Scene : public EntityRegistry {
    ChangeClock clock;
public:
    ChangeClock& getChangeClock() { return clock; }
    ComponentArrayBase* getComponentArray(ComponentId) { return nullptr; }
};

class StructuralDeferScope {
public:
    explicit StructuralDeferScope(Scene&) {}
};

}
