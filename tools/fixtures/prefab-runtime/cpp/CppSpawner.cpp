#include "CppSpawner.h"
#include "CppBullet.h"
#include "GameObject.h"
#include "Log.h"
#include "component/ScriptComponent.h"

using namespace jasperengine;

namespace {

CppBullet* bulletScript(Scene* scene, Entity entity) {
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts) return nullptr;
    for (ScriptEntry& entry : scripts->scripts) {
        if (entry.type == ScriptType::CPP && entry.className == "CppBullet") {
            return static_cast<CppBullet*>(entry.instance);
        }
    }
    return nullptr;
}

}

CppSpawner::CppSpawner(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

CppSpawner::~CppSpawner() {
}

void CppSpawner::onUpdate() {
    frame++;
    time += Engine::getDeltatime();

    auto check = [this](bool condition, const char* what) {
        if (!condition) {
            failures++;
            Log::error("[PRT] C++: %s", what);
        }
    };

    if (frame == 1) {
        check(bullet.isValid(), "the bullet property is not a registered prefab");
        check(bullet.getPath() == "prefabs/Bullet.prefab", "the bullet property has another path");

        const int before = CppBullet::constructed;
        GameObject go = GameObject::instantiate(bullet, Vector3(0, 5, 0), Quaternion());
        if (go.getEntity() == NULL_ENTITY) {
            check(false, "instantiate(bullet) failed");
            return;
        }
        check(go.getName() == "Bullet(Clone)", "the clone is not named Bullet(Clone)");
        check(CppBullet::constructed == before + 1, "the clone's CppBullet did not start");
        CppBullet* script = bulletScript(scene, go.getEntity());
        check(script && script->damage == 7, "the clone's CppBullet damage is not the prefab's 7");

        if (script) {
            script->damage = 9;
        }
        GameObject copy = GameObject::instantiate(go);
        check(CppBullet::constructed == before + 2, "the copy's CppBullet did not start");
        CppBullet* copyScript = bulletScript(scene, copy.getEntity());
        check(copyScript && copyScript != script && copyScript->damage == 9, "the copy's CppBullet damage is not the original's current 9");
        check(copy.getPosition().y == 5.0f, "the copy is not where the original is");

        copyEntity = copy.getEntity();
        GameObject::destroy(copy, 0.2f);
        Log::print("[PRT] C++ instantiated %s and a copy\n", go.getName().c_str());
    }

    if (!done && time > 0.6f) {
        done = true;
        check(!scene->isEntityCreated(copyEntity), "destroy(copy, 0.2) did not remove it");
        check(CppBullet::destroyed >= 1, "the copy's CppBullet was not deleted");
        if (failures == 0) {
            Log::print("[PRT] C++ PASS\n");
        }
    }
}
