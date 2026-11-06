/*
 * Inspired by the work of http://nikitablack.github.io/post/std_function_as_delegate/
 */

// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FUNCTIONSUBSCRIBE_H
#define FUNCTIONSUBSCRIBE_H

#include <cstdint>
#include <functional>
#include <type_traits>
#include <memory>
#include <mutex>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "Log.h"
#include "LuaFunction.h"
#include "util/CrashHook.h"
#include "util/Signal.h"
#include "util/SubscriptionOwnerIndex.h"

#define JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD) \
    (std::string(typeid(std::remove_pointer_t<decltype(this)>).name()) + \
     "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + \
     "_" #METHOD)

// The REGISTER_* macros of C++ scripts: the subscription's owner is the script (this), so
// SubscriptionOwnerIndex::releaseOwner removes them all when the script goes away
#define REGISTER_EVENT(EVENT_OBJ, METHOD) \
    do { \
        std::string __tag = JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD); \
        (EVENT_OBJ).add< \
            std::remove_pointer_t<decltype(this)>, \
            &std::remove_pointer_t<decltype(this)>::METHOD \
        >(__tag, this, ::jasperengine::ownerKeyOf(this)); \
    } while (0)

#define UNREGISTER_EVENT(EVENT_OBJ, METHOD) \
    do { \
        std::string __tag = JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD); \
        (EVENT_OBJ).remove(__tag); \
    } while (0)

#define REGISTER_ENGINE_EVENT(EVENT) \
    do { \
        std::string __tag = JASPERENGINE_EVENT_SUBSCRIPTION_TAG(EVENT); \
        ::jasperengine::Engine::EVENT.add(__tag, ::jasperengine::detail::engineEventCallback( \
            this, &std::remove_pointer_t<decltype(this)>::EVENT), ::jasperengine::ownerKeyOf(this)); \
    } while (0)

#define UNREGISTER_ENGINE_EVENT(EVENT) \
    UNREGISTER_EVENT(::jasperengine::Engine::EVENT, EVENT)

#define REGISTER_COMPONENT_EVENT(COMPONENT, EVENT, METHOD) \
    do { \
        auto* __scene = this->getScene(); \
        if (__scene) { \
            auto* __component = __scene->findComponent<COMPONENT>(this->getEntity()); \
            if (__component) { \
                std::string __tag = JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD); \
                __component->EVENT.add< \
                    std::remove_pointer_t<decltype(this)>, \
                    &std::remove_pointer_t<decltype(this)>::METHOD \
                >(__tag, this, ::jasperengine::ownerKeyOf(this)); \
            } else { \
                ::jasperengine::Log::warn("Cannot register " #COMPONENT "::" #EVENT ": component not found"); \
            } \
        } \
    } while (0)

#define UNREGISTER_COMPONENT_EVENT(COMPONENT, EVENT, METHOD) \
    do { \
        auto* __scene = this->getScene(); \
        if (__scene) { \
            auto* __component = __scene->findComponent<COMPONENT>(this->getEntity()); \
            if (__component) { \
                UNREGISTER_EVENT(__component->EVENT, METHOD); \
            } \
        } \
    } while (0)

#define REGISTER_UI_EVENT(EVENT, METHOD) REGISTER_COMPONENT_EVENT(::jasperengine::UIComponent, EVENT, METHOD)
#define UNREGISTER_UI_EVENT(EVENT, METHOD) UNREGISTER_COMPONENT_EVENT(::jasperengine::UIComponent, EVENT, METHOD)

#define REGISTER_BUTTON_EVENT(EVENT, METHOD) REGISTER_COMPONENT_EVENT(::jasperengine::ButtonComponent, EVENT, METHOD)
#define UNREGISTER_BUTTON_EVENT(EVENT, METHOD) UNREGISTER_COMPONENT_EVENT(::jasperengine::ButtonComponent, EVENT, METHOD)

#define REGISTER_SCROLLBAR_EVENT(EVENT, METHOD) REGISTER_COMPONENT_EVENT(::jasperengine::ScrollbarComponent, EVENT, METHOD)
#define UNREGISTER_SCROLLBAR_EVENT(EVENT, METHOD) UNREGISTER_COMPONENT_EVENT(::jasperengine::ScrollbarComponent, EVENT, METHOD)

#define REGISTER_PANEL_EVENT(EVENT, METHOD) REGISTER_COMPONENT_EVENT(::jasperengine::PanelComponent, EVENT, METHOD)
#define UNREGISTER_PANEL_EVENT(EVENT, METHOD) UNREGISTER_COMPONENT_EVENT(::jasperengine::PanelComponent, EVENT, METHOD)

namespace jasperengine {

    template<typename T>
    class FunctionSubscribe;

    // An engine event: tagged subscribers (adding a tag again replaces it), callable from C++
    // and Lua (the FunctionSubscribe_* bindings). Built on detail::SlotList, Signal's core:
    // removing or adding subscribers from inside a callback is safe (see SlotList), and
    // invoking allocates nothing. While a CrashHook is set every callback runs under the crash
    // guard; one that faults or throws is removed and reported with its tag; without a hook a
    // C++ exception is logged (CrashHook::logFailure) and the subscriber stays. Like Signal, externally
    // synchronized (PhysicsSystem invokes its events from Jolt threads under a mutex);
    // hasSubscribers() is safe from any thread. Copying copies the subscribers.
    template<typename Ret, typename ...Args>
    class FunctionSubscribe<Ret(Args...)> {

    private:
        using List = detail::SlotList<Ret(Args...)>;
        using Slot = typename List::Slot;

        std::shared_ptr<List> state;
        bool enabled = true;

        List& list() {
            if (!state) state = std::make_shared<List>();
            return *state;
        }

        void detach() {
            if (state) {
                state->orphan();
                state.reset();
            }
        }

        // One callback, under the crash guard when a hook is set. False when it faulted or
        // threw: with a hook it is removed and reported; without one (an exported game) a C++
        // exception is logged and the subscriber stays (Unity: Update runs again next frame).
        template<typename Call>
        bool runSlot(List& slots, Slot& slot, bool guarded, Call&& call) {
            if (!guarded) {
                try {
                    call(slot.function);
                } catch (...) {
                    CrashHook::logFailure(slot.tag.empty() ? std::string("event subscriber") : slot.tag);
                    return false;
                }
                return true;
            }
            struct Context {
                Call* call;
                Slot* slot;
            } context{&call, &slot};
            std::string errorInfo;
            const bool ok = CrashHook::runGuarded([](void* data) {
                Context* c = static_cast<Context*>(data);
                (*c->call)(c->slot->function);
            }, &context, errorInfo);
            if (!ok) {
                const std::string tag = slot.tag;
                slots.removeSlot(slot.id);
                CrashHook::report(tag, errorInfo);
            }
            return ok;
        }

    public:

        FunctionSubscribe() = default;

        FunctionSubscribe(const FunctionSubscribe& other): enabled(other.enabled) {
            if (other.state) state = other.state->clone();
        }

        FunctionSubscribe& operator = (const FunctionSubscribe& other) {
            if (this != &other) {
                detach();
                enabled = other.enabled;
                if (other.state) state = other.state->clone();
            }
            return *this;
        }

        FunctionSubscribe(FunctionSubscribe&& other) noexcept: state(std::move(other.state)), enabled(other.enabled) {}

        FunctionSubscribe& operator = (FunctionSubscribe&& other) noexcept {
            if (this != &other) {
                detach();
                state = std::move(other.state);
                enabled = other.enabled;
            }
            return *this;
        }

        ~FunctionSubscribe() {
            detach();
        }

        FunctionSubscribe(std::function<Ret(Args...)> function){
            add("cFunction", function);
        }

        FunctionSubscribe(lua_State *L) {
            add("luaFunction", L);
        }

        FunctionSubscribe& operator = (std::function<Ret(Args...)> function){
            add("cFunction", function);

            return *this;
        }

        FunctionSubscribe& operator = (lua_State *L){
            add("luaFunction", L);

            return *this;
        }

        void setEnabled(bool enabled) {
            this->enabled = enabled;
        }

        bool isEnabled() const {
            return enabled;
        }

        bool hasSubscribers() const {
            return state && state->size() > 0;
        }

        size_t size() const {
            return state ? state->size() : 0;
        }

        // The Lua binding (event:add(tag, function)): owned by the script RegisterEvent is
        // registering for (SubscriptionOwnerIndex::registrationOwner), if any
        bool add(const std::string& tag, lua_State *L){
            std::function<Ret(Args...)> function = LuaFunction<Ret>(L);
            return add(tag, function, SubscriptionOwnerIndex::registrationOwner());
        }

        // The live subscribers that are Lua functions (physics runs a 3D world on the main
        // thread while its shouldCollide3D has one): the ones add(tag, lua_State*) made
        size_t luaSubscriberCount() const {
            size_t count = 0;
            if (state) {
                state->invoke([&count](Slot& slot) {
                    if (slot.function.target_type() == typeid(LuaFunction<Ret>)) count++;
                    return true;
                });
            }
            return count;
        }

        bool add(const std::string& tag, std::function<Ret(Args...)> function, uint64_t ownerKey = 0) {
            List& slots = list();
            slots.removeTag(tag);
            slots.add(std::move(function), ownerKey, tag);
            return true;
        }

        template<Ret(*funcPtr)(Args...)>
        bool add(const std::string& tag, uint64_t ownerKey = 0){
            return add(tag, std::function<Ret(Args...)>(funcPtr), ownerKey);
        }

        template<typename T, Ret(T::*funcPtr)(Args...)>
        bool add(const std::string& tag, std::shared_ptr<T> obj, uint64_t ownerKey = 0){
            T* target = obj.get();
            return add(tag, std::function<Ret(Args...)>([target](Args... args) -> Ret { return (target->*funcPtr)(args...); }),
                       ownerKey);
        }

        template<typename T, Ret(T::*funcPtr)(Args...)>
        bool add(const std::string& tag, T* obj, uint64_t ownerKey = 0){
            return add(tag, std::function<Ret(Args...)>([obj](Args... args) -> Ret { return (obj->*funcPtr)(args...); }),
                       ownerKey);
        }

        template<typename T>
        bool add(const std::string& tag, std::shared_ptr<T> t, uint64_t ownerKey = 0){
            return add(tag, std::function<Ret(Args...)>(*t.get()), ownerKey);
        }

        // An untagged subscriber held by the returned Subscription (RAII)
        [[nodiscard]] Subscription subscribe(std::function<Ret(Args...)> function, uint64_t ownerKey = 0) {
            const uint64_t id = list().add(std::move(function), ownerKey, std::string());
            return Subscription(std::weak_ptr<detail::SlotStateBase>(state), id);
        }

        // A probe of the subscription `tag` (empty when there is none): true while some copy of
        // this event still lists it (not removed, cleared or destroyed with its component)
        // (the list of this object, which a move keeps; a copy has a list of its own)
        std::function<bool()> subscriptionProbe(const std::string& tag) const {
            if (!state || !state->hasTag(tag)) {
                return {};
            }
            std::weak_ptr<List> weak = state;
            return [weak, tag]() {
                std::shared_ptr<List> alive = weak.lock();
                return alive && alive->hasTag(tag);
            };
        }

        bool remove(const std::string& tag){
            return state && state->removeTag(tag);
        }

        // A live subscription with this tag (RegisterEvent / add)
        bool has(const std::string& tag) const{
            return state && state->hasTag(tag);
        }

        size_t removeByTagSubstring(const std::string& substring){
            return state ? state->removeTagSubstring(substring) : 0;
        }

        size_t removeOwner(uint64_t ownerKey) {
            return state ? state->removeOwner(ownerKey) : 0;
        }

        // Every live subscriber, without the crash guard (process-wide signal handlers: not for
        // callers on several threads at once, as Jolt's job threads are). Void events only.
        // SlotList's invoke is not for concurrent callers: the calls of one signature take
        // turns on a lock (subscribers are added and removed outside those calls)
        void callDirect(Args... args){
            static_assert(std::is_void<Ret>::value, "callDirect is for void events");
            if (!enabled || !state){
                return;
            }
            static std::mutex directMutex;
            std::lock_guard<std::mutex> lock(directMutex);
            state->invoke([&](Slot& slot) {
                slot.function(args...);
                return true;
            });
        }

        Ret call(Args... args){
            if constexpr (std::is_void<Ret>::value) {
                if (!enabled || !state) return;
                // the list, not the member: a subscriber may destroy this event
                List* slots = state.get();
                const bool guarded = CrashHook::active();
                slots->invoke([&](Slot& slot) {
                    runSlot(*slots, slot, guarded, [&](std::function<Ret(Args...)>& function) { function(args...); });
                    return true;
                });
            } else {
                return callRet(args..., Ret());
            }
        }

        // The first subscriber's result (a faulting one is removed and the next one asked);
        // def without subscribers or while disabled
        template<typename T>
        Ret callRet(Args... args, T def){
            if (!enabled || !state) {
                return def;
            }
            List* slots = state.get();
            const bool guarded = CrashHook::active();
            Ret result = def;
            slots->invoke([&](Slot& slot) {
                Ret value{};
                const bool ok = runSlot(*slots, slot, guarded, [&](std::function<Ret(Args...)>& function) {
                    value = function(args...);
                });
                if (ok) {
                    result = value;
                    return false;
                }
                return true;
            });
            return result;
        }

        Ret operator()(Args... args){
            return call(args...);
        }

        void clear(){
            if (state) state->clear();
        }
    };

    class Scene;

    // True when a script instance may receive engine events: its GameObject is active
    // in hierarchy, its script entry is enabled, and it is not enabled-but-not-started yet
    // (start runs before its first update; defined in ScriptBase.cpp).
    JASPERENGINE_API bool isScriptActive(Scene* scene, unsigned int entity, const void* instance);

    namespace detail {
        template<typename T, typename = void>
        struct IsSceneScript : std::false_type {};

        template<typename T>
        struct IsSceneScript<T, std::void_t<decltype(std::declval<T*>()->getScene()), decltype(std::declval<T*>()->getEntity())>> : std::true_type {};

        // REGISTER_ENGINE_EVENT callback; a scene script (ScriptBase) is skipped while
        // inactive or disabled, like Unity's Update on a disabled MonoBehaviour
        template<typename T, typename Ret, typename... Args>
        std::function<Ret(Args...)> engineEventCallback(T* obj, Ret (T::*method)(Args...)) {
            if constexpr (IsSceneScript<T>::value) {
                return [obj, method](Args... args) -> Ret {
                    if (!::jasperengine::isScriptActive(obj->getScene(), obj->getEntity(), static_cast<const void*>(obj))) {
                        if constexpr (std::is_void<Ret>::value) {
                            return;
                        } else {
                            return Ret();
                        }
                    }
                    return (obj->*method)(args...);
                };
            } else {
                return [obj, method](Args... args) -> Ret {
                    return (obj->*method)(args...);
                };
            }
        }
    }
}

#endif //FUNCTIONSUBSCRIBE_H
