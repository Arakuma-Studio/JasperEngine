#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "ScriptProperty.h"
#include "PrefabAsset.h"

// The C++ side of the runtime prefab test: a Prefab property, GameObject::instantiate and
// GameObject::destroy, and the C++ scripts of the copies
class CppSpawner : public jasperengine::ScriptBase {
public:
    DPROPERTY("Bullet")
    jasperengine::PrefabAsset bullet;

    CppSpawner(jasperengine::Scene* scene, jasperengine::Entity entity);
    ~CppSpawner();

    void onUpdate();

private:
    int frame = 0;
    float time = 0;
    bool done = false;
    int failures = 0;
    jasperengine::Entity copyEntity = NULL_ENTITY;
};
