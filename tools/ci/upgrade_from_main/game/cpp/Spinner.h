// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ script fixture of the upgrade test (tools/ci/upgrade_from_main): spins a target,
// moves its own GameObject, prints the same kind of transcript the Lua game does.
#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "GameObject.h"
#include "ScriptProperty.h"

class Spinner : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 90.0f;

    DPROPERTY("Drift")
    jasperengine::Vector3 drift = jasperengine::Vector3(0.5f, 0.0f, 0.0f);

    DPROPERTY("Target")
    jasperengine::GameObject* target = nullptr;

    DPROPERTY("Frames")
    int frames = 120;

    Spinner(jasperengine::Scene* scene, jasperengine::Entity entity);
    virtual ~Spinner();

    void onUpdate();
    void onPostUpdate();

private:
    int frame = 0;
    float angle = 0.0f;
};
