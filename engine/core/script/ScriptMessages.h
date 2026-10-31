// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity-style per-object script messages: a method of a GameObject's scripts called by name when
// the script defines it (onJointBreak2D...). The contract of physics-contacts 6.1.1, shared with
// script-lifecycle, which add their messages and the receiver cache on top.
// - Main thread only (the Lua state is single-threaded): physics queues what happened in a step
//   and sends after it.
// - Every script instance of the GameObject that defines the method gets it, enabled or not
//   (Unity), in the ScriptComponent order, Lua then C++. The components are read again after
//   each call: a callback may add or remove components or destroy the GameObject.
// - Errors are logged with a traceback and never propagate.

#ifndef SCRIPT_MESSAGES_H
#define SCRIPT_MESSAGES_H

#include "Export.h"
#include "ecs/Entity.h"
#include "script/ScriptBase.h"

#include <cstdint>
#include <functional>
#include <type_traits>

typedef struct lua_State lua_State;

namespace jasperengine {

    class Scene;

    enum class ScriptMessage : uint8_t {
        CollisionEnter, CollisionStay, CollisionExit, TriggerEnter, TriggerStay, TriggerExit,
        CollisionEnter2D, CollisionStay2D, CollisionExit2D, TriggerEnter2D, TriggerStay2D, TriggerExit2D,
        JointBreak, JointBreak2D,
        // script-lifecycle appends its own (Enable, Disable, ...) after these
        Count
    };
    using ScriptMessageMask = uint32_t;          // bit (1u << (int)message)

    constexpr ScriptMessageMask scriptMessageBit(ScriptMessage message){
        return 1u << (uint32_t)message;
    }

    class JASPERENGINE_API ScriptMessages {
    public:
        // "onCollisionEnter", ..., "onJointBreak2D"
        static const char* methodName(ScriptMessage m);

        // The messages some script instance of the GameObject defines (Lua: a function in the
        // instance table or its class; C++: cppMessageMask less the messages the script was seen
        // not to override, so a C++ script counts until a message reached its base method)
        static ScriptMessageMask receivers(Scene* scene, Entity entity);

        // Calls the method on every script instance of the GameObject that defines it (see the
        // header). pushArgs pushes the Lua arguments after self and returns their count; callCpp
        // calls the C++ virtual. Returns the number of methods called.
        static int send(Scene* scene, Entity entity, ScriptMessage m,
                        const std::function<int(lua_State*)>& pushArgs,
                        const std::function<void(ScriptBase&)>& callCpp);

        // Told when receivers(scene, entity) may have changed
        using ReceiversChanged = void (*)(Scene* scene, Entity entity);
        static void addReceiversChangedListener(ReceiversChanged listener);
        static void removeReceiversChangedListener(ReceiversChanged listener);
        // the scripts of the GameObject changed (an instance made, released, enabled, disabled)
        static void notifyReceiversChanged(Scene* scene, Entity entity);

        // The messages a C++ script T may receive (generated script factories store it in
        // ScriptEntry::cppMessageMask): all of them for a ScriptBase, none otherwise. Whether T
        // overrides one is learned when it is first sent (the base method marks
        // ScriptBase::inheritedMessages), without naming T's members here (an override may be
        // protected, private or overloaded)
        template<typename T>
        static constexpr ScriptMessageMask cppMask(){
            if constexpr (std::is_base_of_v<ScriptBase, T>) {
                return (ScriptMessageMask)((1ull << (uint32_t)ScriptMessage::Count) - 1);
            } else {
                return 0;
            }
        }

        // The ScriptBase of a C++ script instance, null when T is not one
        template<typename T>
        static ScriptBase* cppScript(T* instance){
            if constexpr (std::is_base_of_v<ScriptBase, T>) {
                return static_cast<ScriptBase*>(instance);
            } else {
                (void)instance;
                return nullptr;
            }
        }
    };

}

#endif //SCRIPT_MESSAGES_H
