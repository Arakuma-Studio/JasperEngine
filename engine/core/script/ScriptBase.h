// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"
#include "ScriptProperty.h"

#include <map>
#include <string>

namespace jasperengine {

    // What a C++ script keeps across a reload during Play (Preferences > Scripting > Reload
    // C++ Scripts During Play): values by name, and a blob for anything else. DPROPERTY
    // members are carried over without it; other members are lost unless saved here.
    struct ScriptReloadState {
        std::map<std::string, ScriptPropertyValue> values;
        std::string blob;
    };

    class JASPERENGINE_API ScriptBase {
    protected:
        Scene* scene;
        Entity entity;

    public:
        ScriptBase(Scene* scene, Entity entity);
        virtual ~ScriptBase() = default;

        // A C++ reload during Play: the old instance saves, the new one (constructed with
        // isReloading() true, its DPROPERTYs already carried over) loads
        virtual void saveReloadState(ScriptReloadState& state) const {}
        virtual void loadReloadState(const ScriptReloadState& state) {}
        // True while a reload constructs the new instances: a constructor with side effects
        // (spawning, sounds) can skip them
        static bool isReloading();
        // The editor's reload sets it around the new instances' construction
        static void setReloading(bool reloading);

        Scene* getScene() const;
        Entity getEntity() const;

        // Unity isActiveAndEnabled: the GameObject is active in hierarchy and this
        // script's entry is enabled. Engine events registered with
        // REGISTER_ENGINE_EVENT are skipped while it is false.
        bool isActiveAndEnabled() const;
    };

}

#endif //SCRIPT_BASE_H