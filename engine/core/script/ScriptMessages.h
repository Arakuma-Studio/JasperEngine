// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_MESSAGES_H
#define SCRIPT_MESSAGES_H

#include "Export.h"
#include "ecs/Entity.h"
#include "animation/AnimationEvent.h"
#include "ScriptBase.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    class Scene;

    // The per-object messages a script receives by method name, like Unity's MonoBehaviour
    // messages: Lua calls `self:<methodName>(...)` when the script defines it, C++ calls the
    // ScriptBase virtual when the class overrides it. One mechanism for the lifecycle
    // (script-lifecycle), the physics contacts (physics-contacts) and the UI pointer events.
    // The physics values keep their order; features append theirs before Count (< 32).
    enum class ScriptMessage : uint8_t {
        CollisionEnter, CollisionStay, CollisionExit, TriggerEnter, TriggerStay, TriggerExit,
        CollisionEnter2D, CollisionStay2D, CollisionExit2D, TriggerEnter2D, TriggerStay2D, TriggerExit2D,
        JointBreak, JointBreak2D,
        // script-lifecycle
        Init, Start, Enable, Disable, Destroy, ApplicationPause, ApplicationFocus, ApplicationQuit,
        Count
    };
    static_assert(static_cast<int>(ScriptMessage::Count) <= 32, "ScriptMessageMask holds 32 messages");

    using ScriptMessageMask = uint32_t;          // bit (1u << (int)message)

    constexpr ScriptMessageMask scriptMessageBit(ScriptMessage m) {
        return 1u << static_cast<unsigned>(m);
    }

    // What a delivery requires besides a live instance (send's `deliver` argument)
    enum ScriptDelivery : uint32_t {
        DeliverAny = 0,
        DeliverRequireActive = 1u << 0,   // the GameObject is active in hierarchy (else nothing is sent)
        DeliverRequireEnabled = 1u << 1,  // only enabled script entries
    };

    // Which lifecycle virtuals a C++ script class overrides: &T::name has class type T then.
    // A name T overloads (no single &T::name) counts as overridden: the virtual is called, which
    // is right either way.
#define JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(NAME) \
    template<class T, class = void> struct ScriptOverrides_##NAME : std::true_type {}; \
    template<class T> struct ScriptOverrides_##NAME<T, std::void_t<decltype(&T::NAME)>> \
        : std::bool_constant<!std::is_same_v<decltype(&T::NAME), decltype(&ScriptBase::NAME)>> {};
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(init)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(start)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onEnable)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onDisable)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onDestroy)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onApplicationPause)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onApplicationFocus)
    JASPERENGINE_SCRIPT_OVERRIDE_TRAIT(onApplicationQuit)
#undef JASPERENGINE_SCRIPT_OVERRIDE_TRAIT

    class JASPERENGINE_API ScriptMessages {
    public:
        // "onCollisionEnter", ..., "init", "start", "onEnable", "onDisable", "onDestroy",
        // "onApplicationPause", "onApplicationFocus", "onApplicationQuit"
        static const char* methodName(ScriptMessage m);

        // The messages some live script instance of the GameObject defines (Lua: a function in
        // the instance or its class; C++: an override, ScriptEntry::cppMessageMask), enabled or
        // not. Cached per GameObject, recomputed after notifyReceiversChanged.
        static ScriptMessageMask receivers(Scene* scene, Entity entity);

        // Calls the method on every script instance of the GameObject that defines it, in the
        // Script component's order, enabled or not (unless `deliver` asks otherwise), skipping
        // instances never initialized (the GameObject was never active) or already destroyed.
        // pushArgs pushes the Lua arguments and returns their count (may be null: none);
        // callCpp calls the C++ virtual (may be null: Lua only). The component is read again
        // after each call (a callback may add scripts, instantiate or destroy the GameObject);
        // errors are logged with a traceback and never propagate. Returns the number of methods
        // called. Main thread only.
        static int send(Scene* scene, Entity entity, ScriptMessage m,
                        const std::function<int(lua_State*)>& pushArgs,
                        const std::function<void(ScriptBase&)>& callCpp,
                        uint32_t deliver = DeliverAny);
        // One entry only (the lifecycle targets a single script); the same rules
        static bool sendTo(Scene* scene, Entity entity, size_t scriptIndex, ScriptMessage m,
                           const std::function<int(lua_State*)>& pushArgs,
                           const std::function<void(ScriptBase&)>& callCpp);
        // A method given by name on the enabled Lua scripts of the GameObject (the Animation
        // Player's and Vertex Animator's clip events). True when at least one defines it.
        static bool sendNamed(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args);
        // One entry without any delivery rule (ScriptRuntime applies the lifecycle's own). True
        // when the script defines the method (it was called; an error in it is logged).
        static bool invoke(Scene* scene, Entity entity, size_t scriptIndex, ScriptMessage m,
                           const std::function<int(lua_State*)>& pushArgs,
                           const std::function<void(ScriptBase&)>& callCpp);

        // Told when receivers(scene, entity) may have changed
        using ReceiversChanged = void (*)(Scene* scene, Entity entity);
        static void addReceiversChangedListener(ReceiversChanged listener);
        // A script instance of the GameObject was created, released, enabled or disabled
        static void notifyReceiversChanged(Scene* scene, Entity entity);
        // The scene goes: its cached masks with it
        static void forgetScene(Scene* scene);

        // The generated factory's C++ bookkeeping for a script class T
        template<class T>
        static constexpr ScriptMessageMask cppMask() {
            if constexpr (std::is_base_of_v<ScriptBase, T>) {
                ScriptMessageMask mask = 0;
                if (ScriptOverrides_init<T>::value) mask |= scriptMessageBit(ScriptMessage::Init);
                if (ScriptOverrides_start<T>::value) mask |= scriptMessageBit(ScriptMessage::Start);
                if (ScriptOverrides_onEnable<T>::value) mask |= scriptMessageBit(ScriptMessage::Enable);
                if (ScriptOverrides_onDisable<T>::value) mask |= scriptMessageBit(ScriptMessage::Disable);
                if (ScriptOverrides_onDestroy<T>::value) mask |= scriptMessageBit(ScriptMessage::Destroy);
                if (ScriptOverrides_onApplicationPause<T>::value) mask |= scriptMessageBit(ScriptMessage::ApplicationPause);
                if (ScriptOverrides_onApplicationFocus<T>::value) mask |= scriptMessageBit(ScriptMessage::ApplicationFocus);
                if (ScriptOverrides_onApplicationQuit<T>::value) mask |= scriptMessageBit(ScriptMessage::ApplicationQuit);
                return mask;
            } else {
                return 0;
            }
        }
        // The ScriptBase of a script object, null for a class that is not one (the GameObject
        // wrapper kind of C++ script gets no messages)
        template<class T>
        static ScriptBase* asScriptBase(T* script) {
            if constexpr (std::is_base_of_v<ScriptBase, T>) {
                return static_cast<ScriptBase*>(script);
            } else {
                (void)script;
                return nullptr;
            }
        }
    };

}

#endif // SCRIPT_MESSAGES_H
