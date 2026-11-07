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
    // ScriptBase virtual when the class overrides it. Main thread only (physics queues what a
    // step produced and sends after it); errors are logged with a traceback, never propagate. One mechanism for the lifecycle
    // (script-lifecycle), the physics contacts (physics-contacts) and the UI pointer events.
    // The physics values keep their order; features append theirs before Count (< 64).
    enum class ScriptMessage : uint8_t {
        CollisionEnter, CollisionStay, CollisionExit, TriggerEnter, TriggerStay, TriggerExit,
        CollisionEnter2D, CollisionStay2D, CollisionExit2D, TriggerEnter2D, TriggerStay2D, TriggerExit2D,
        JointBreak, JointBreak2D,
        // script-lifecycle
        Init, Start, Enable, Disable, Destroy, ApplicationPause, ApplicationFocus, ApplicationQuit,
        // particles: a version 2 Particle System with stopAction Callback finished
        ParticleSystemStopped,
        // ui-multitouch: the EventSystem callbacks of the component UI (a PointerEventData argument)
        PointerDown, PointerUp, PointerClick, PointerEnter, PointerExit, InitializePotentialDrag,
        BeginDrag, Drag, EndDrag, Drop, Scroll,
        // ui-components: the selection callbacks (a BaseEventData or AxisEventData argument)
        Select, Deselect, Move, Submit, Cancel,
        Count
    };
    static_assert(static_cast<int>(ScriptMessage::Count) < 64, "ScriptMessageMask holds 63 messages (all-ones mask shifts by Count)");

    using ScriptMessageMask = uint64_t;          // bit (1 << (int)message)

    constexpr ScriptMessageMask scriptMessageBit(ScriptMessage m) {
        return static_cast<ScriptMessageMask>(1) << static_cast<unsigned>(m);
    }

    // What a delivery requires besides a live instance (send's `deliver` argument)
    enum ScriptDelivery : uint32_t {
        DeliverAny = 0,
        DeliverRequireActive = 1u << 0,   // the GameObject is active in hierarchy (else nothing is sent)
        DeliverRequireEnabled = 1u << 1,  // only enabled script entries
    };

    class JASPERENGINE_API ScriptMessages {
    public:
        // "onCollisionEnter", ..., "init", "start", "onEnable", "onDisable", "onDestroy",
        // "onApplicationPause", "onApplicationFocus", "onApplicationQuit", "onPointerDown", ...
        static const char* methodName(ScriptMessage m);
        // the message of a method name; false for other names
        static bool fromMethodName(const char* name, ScriptMessage& out);

        // Whether a live, enabled entry of the GameObject defines the message (Lua: a function in
        // the instance or its class, read now; C++: an override), skipping the Lua instances that
        // subscribed a method of that name to a component event (isLegacySubscription). The
        // EventSystem's handler search (ScriptRuntime::findHandler).
        static bool defines(Scene* scene, Entity entity, ScriptMessage m);

        // ui-multitouch D4's collision rule: a Lua instance that subscribed its method `name` to a
        // UIComponent pointer event with RegisterEvent (the old `onPointerDown(x, y)` pattern) is
        // not also called with a PointerEventData under that name while one such subscription
        // lives. Keyed by the instance table; each note keeps a probe of its subscription
        // (FunctionSubscribe::subscriptionProbe), so an unregistered, cleared or destroyed
        // event's note no longer counts.
        static void noteLegacySubscription(const void* instanceTable, const std::string& method,
                                           std::function<bool()> subscriptionAlive);
        static bool isLegacySubscription(const void* instanceTable, const char* method);
        // the instance is released: its notes go (the address can be reused)
        static void forgetLegacySubscriptions(const void* instanceTable);

        // The messages some live script instance of the GameObject defines (Lua: a function in
        // the instance or its class; C++: an override, ScriptEntry::cppMessageMask), enabled or
        // not. Cached per GameObject, recomputed after notifyReceiversChanged.
        static ScriptMessageMask receivers(Scene* scene, Entity entity);

        // Calls the method on every script instance of the GameObject that defines it, Lua ones
        // then C++ ones, each in the Script component's order, enabled or not (unless `deliver`
        // asks otherwise), skipping
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
        // Player's and Vertex Animator's clip events, its controller's onStateEnter/Exit). True
        // when at least one defines it. `integers`: whole numbers go as Lua integers
        static bool sendNamed(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args,
                              bool integers = false);
        // One entry without any delivery rule (ScriptRuntime applies the lifecycle's own). True
        // when the script defines the method (it was called; an error in it is logged).
        static bool invoke(Scene* scene, Entity entity, size_t scriptIndex, ScriptMessage m,
                           const std::function<int(lua_State*)>& pushArgs,
                           const std::function<void(ScriptBase&)>& callCpp);

        // The messages of `filter` each script of the GameObject defines, as
        // "onTriggerEnter (Coin)" (the Inspector's Callbacks line, get_runtime_entity)
        static std::vector<std::string> describeReceivers(Scene* scene, Entity entity, ScriptMessageMask filter);

        // Told when receivers(scene, entity) may have changed
        using ReceiversChanged = void (*)(Scene* scene, Entity entity);
        static void addReceiversChangedListener(ReceiversChanged listener);
        static void removeReceiversChangedListener(ReceiversChanged listener);
        // A script instance of the GameObject was created, released, enabled or disabled
        static void notifyReceiversChanged(Scene* scene, Entity entity);
        // The scene goes: its cached masks with it
        static void forgetScene(Scene* scene);

        // The messages a C++ script class T may receive (the generated factory stores it in
        // ScriptEntry::cppMessageMask): all of them for a ScriptBase, none otherwise. Whether T
        // overrides one is learned the first time it is sent (the base method marks
        // ScriptBase::inheritedMessages: not sent again, not among receivers), without naming
        // T's members here (an override may be protected, private or overloaded)
        template<class T>
        static constexpr ScriptMessageMask cppMask() {
            if constexpr (std::is_base_of_v<ScriptBase, T>) {
                return static_cast<ScriptMessageMask>((1ull << static_cast<unsigned>(ScriptMessage::Count)) - 1);
            } else {
                return 0;
            }
        }
        // The ScriptBase of a C++ script instance, null when T is not one (physics-2d's name)
        template<class T>
        static ScriptBase* cppScript(T* instance) {
            return asScriptBase(instance);
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
