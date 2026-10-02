#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "ScriptProperty.h"

// On prefabs/Bullet.prefab, a prefab no scene holds: its C++ script still runs on the
// copies scripts instantiate
class CppBullet : public jasperengine::ScriptBase {
public:
    DPROPERTY("Damage")
    int damage = 1;

    static int constructed;
    static int destroyed;

    CppBullet(jasperengine::Scene* scene, jasperengine::Entity entity);
    ~CppBullet();
};
