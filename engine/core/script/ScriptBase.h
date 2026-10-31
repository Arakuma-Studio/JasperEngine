// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"

namespace jasperengine {

    class Joint2D;

    class JASPERENGINE_API ScriptBase {
    protected:
        Scene* scene;
        Entity entity;

    public:
        ScriptBase(Scene* scene, Entity entity);
        virtual ~ScriptBase() = default;

        Scene* getScene() const;
        Entity getEntity() const;

        // Unity isActiveAndEnabled: the GameObject is active in hierarchy and this
        // script's entry is enabled. Engine events registered with
        // REGISTER_ENGINE_EVENT are skipped while it is false.
        bool isActiveAndEnabled() const;

        // Script messages (ScriptMessages), like the Lua methods of the same name: override them
        // (public, protected or private) and do not call the base, which only notes the message
        // is not overridden. A 2D joint of this GameObject (or whose bodyA is it) broke (Unity
        // OnJointBreak2D); the joint still exists during the call.
        virtual void onJointBreak2D(Joint2D& joint);

        // the messages (ScriptMessage bits) whose base method ran: not overridden, so not sent
        // again and not among ScriptMessages::receivers
        uint32_t inheritedMessages = 0;
        // set by a base message method (ScriptMessages reads it after a call without touching a
        // script the call may have destroyed)
        static bool baseMessageRan;
    };

}

#endif //SCRIPT_BASE_H