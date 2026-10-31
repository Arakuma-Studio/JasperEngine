// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"
#include "component/ScriptComponent.h"

namespace jasperengine {

    class Joint2D;

    // The base of a C++ behaviour script (Unity's MonoBehaviour). The engine calls the
    // lifecycle methods below when a subclass overrides them, with the rules of Lua scripts
    // (docs/mcp/guide/cpp-scripting.md): init once when the GameObject is first active in
    // hierarchy (Unity Awake, after the properties are bound), onEnable / onDisable when it
    // becomes / stops being active and enabled, start once before its first update, onDestroy
    // once (destroy, scene unload, Play stop / quit), and the application messages.
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

        // Unity's enabled: this script's entry in the Script component. Setting it calls
        // onEnable / onDisable (once the scene's scripts started); same value: nothing.
        bool isEnabled() const;
        void setEnabled(bool enabled);

        // Unity GetComponent<T>(): the first C++ script of this GameObject that is a T
        // (dynamic_cast, so a base class finds its subclasses), enabled or not; null if none
        template<class T>
        T* getScript() const {
            if (!scene || !scene->isEntityCreated(entity)) {
                return nullptr;
            }
            if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
                for (const ScriptEntry& entry : scripts->scripts) {
                    if (entry.type == ScriptType::CPP && entry.cppScript) {
                        if (T* script = dynamic_cast<T*>(entry.cppScript)) {
                            return script;
                        }
                    }
                }
            }
            return nullptr;
        }

        // Script messages (ScriptMessages), like the Lua methods of the same name: override what
        // the script needs (public, protected or private) and do not call the base, which only
        // notes the message is not overridden (it is not sent again).
        // Lifecycle (script-lifecycle spec 7.2):
        virtual void init();
        virtual void start();
        virtual void onEnable();
        virtual void onDisable();
        virtual void onDestroy();
        virtual void onApplicationPause(bool paused);
        virtual void onApplicationFocus(bool focused);
        virtual void onApplicationQuit();

        // A 2D joint of this GameObject (or whose bodyA is it) broke (Unity OnJointBreak2D); the
        // joint still exists during the call.
        virtual void onJointBreak2D(Joint2D& joint);

        // A Particle System of this GameObject with main.stopAction Callback finished: it stopped
        // and its last particle died (Unity OnParticleSystemStopped).
        virtual void onParticleSystemStopped();

        // the messages (ScriptMessage bits) whose base method ran: not overridden, so not sent
        // again and not among ScriptMessages::receivers
        uint32_t inheritedMessages = 0;
        // set by a base message method (ScriptMessages reads it after a call without touching a
        // script the call may have destroyed)
        static bool baseMessageRan;
    };

}

#endif //SCRIPT_BASE_H
