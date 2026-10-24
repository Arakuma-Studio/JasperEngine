// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SUBSYSTEM_H
#define SUBSYSTEM_H

#include "Export.h"
#include "Entity.h"
#include "Signature.h"
#include "ComponentManager.h"

namespace jasperengine{

    class Scene;

    class JASPERENGINE_API SubSystem {

        private:
        // The types onComponentAdded/onComponentRemoved handle (declareHookType); every type
        // while a system declares none
        Signature hookTypes;
        bool hookTypesDeclared = false;

        protected:
        Signature signature;
        Scene* scene;
        bool paused = false;

        // Narrows the Scene's add/remove hook calls to the declared types, so that destroying
        // an entity calls only the systems that handle one of its types. Every type the hooks
        // compare the id against must be declared.
        void declareHookType(ComponentId componentId) {
            hookTypes.set(componentId);
            hookTypesDeclared = true;
        }

        // The hooks handle no type (they are not overridden)
        void declareNoHookTypes() {
            hookTypesDeclared = true;
        }

    public:

        SubSystem(Scene* scene) {
            this->scene = scene;
        }

        SubSystem(const SubSystem&) = delete;
        SubSystem& operator=(const SubSystem&) = delete;

        virtual void setPaused(bool value) {
            this->paused = value;
        }

        virtual bool isPaused() const {
            return paused;
        }

        virtual void load() = 0;
        virtual void draw() = 0;
        virtual void destroy() = 0;
        // Variable timestep update. Called once per frame with the real frame delta time.
        // Use for animation, input, UI, gameplay logic that does not need a deterministic step.
        virtual void update(double dt) = 0;
        // Fixed timestep update. Called zero or more times per frame with Engine::getUpdateTime() as dt.
        // Use for physics, networking and any logic requiring a stable, deterministic step.
        virtual void fixedUpdate(double dt) { (void)dt; }

        // whether the Scene calls the add/remove hooks for this type
        bool handlesHookType(ComponentId componentId) const {
            return !hookTypesDeclared || hookTypes.test(componentId);
        }

        // the types of `types` the add/remove hooks handle
        Signature hookTypesOf(const Signature& types) const {
            return hookTypesDeclared ? (types & hookTypes) : types;
        }

        virtual void onComponentAdded(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        virtual void onComponentRemoved(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        // GameObject layer changed (Scene::setEntityLayer)
        virtual void onEntityLayerChanged(Entity entity) { (void)entity; }
        // The scheduler stopped (false) or resumed running the system (the editor's Systems
        // window and set_system_options, a Play-mode debug switch); its phases are skipped
        // meanwhile. A system with effects that outlive its updates (sounds) pauses them.
        virtual void onSchedulerEnabledChanged(bool enabled) { (void)enabled; }

    };

}


#endif //SUBSYSTEM_H