// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"

namespace jasperengine {

    class JASPERENGINE_API ScriptBase {
    protected:
        Scene* scene;
        Entity entity;

    public:
        ScriptBase(Scene* scene, Entity entity);
        ~ScriptBase() = default;

        Scene* getScene() const;
        Entity getEntity() const;

        // Unity isActiveAndEnabled: the GameObject is active in hierarchy and this
        // script's entry is enabled. Engine events registered with
        // REGISTER_ENGINE_EVENT are skipped while it is false.
        bool isActiveAndEnabled() const;
    };

}

#endif //SCRIPT_BASE_H