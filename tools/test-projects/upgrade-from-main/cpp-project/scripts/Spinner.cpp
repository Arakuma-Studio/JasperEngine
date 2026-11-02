// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Spinner.h"
#include "GameObject.h"
#include "Log.h"

using namespace jasperengine;

Spinner::Spinner(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
    REGISTER_ENGINE_EVENT(onPostUpdate);
}

Spinner::~Spinner() {
    UNREGISTER_ENGINE_EVENT(onUpdate);
    UNREGISTER_ENGINE_EVENT(onPostUpdate);
}

void Spinner::onUpdate() {
    const float dt = Engine::getDeltatime();
    angle += speed * dt;
    if (target) {
        target->setRotation(Quaternion(angle, Vector3(0, 1, 0)));
    }
    GameObject self(getScene(), getEntity());
    self.setPosition(self.getPosition() + drift * dt);
}

void Spinner::onPostUpdate() {
    if (frame % 10 == 0 && frame <= frames) {
        GameObject self(getScene(), getEntity());
        const Vector3 p = self.getPosition();
        Log::print("T|Cpp|%d|spin|%.3f|(%.3f,%.3f,%.3f)|%s", frame, angle, p.x, p.y, p.z, target ? "true" : "false");
    }
    if (frame == frames) {
        Log::print("T|Cpp|%d|end", frame);
    }
    frame++;
}
