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
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>
#include <algorithm>

#include "Log.h"
#include "LuaFunction.h"
#ifdef JASPERENGINE_CRASH_GUARD
#include "util/CrashGuard.h"
#endif

#define JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD) \
    (std::string(typeid(std::remove_pointer_t<decltype(this)>).name()) + \
     "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + \
     "_" #METHOD)

#define REGISTER_EVENT(EVENT_OBJ, METHOD) \
    do { \
        std::string __tag = JASPERENGINE_EVENT_SUBSCRIPTION_TAG(METHOD); \
        (EVENT_OBJ).add< \
            std::remove_pointer_t<decltype(this)>, \
            &std::remove_pointer_t<decltype(this)>::METHOD \
        >(__tag, this); \
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
            this, &std::remove_pointer_t<decltype(this)>::EVENT)); \
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
                >(__tag, this); \
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


template<size_t>
struct MyPlaceholder {};

template<size_t N>
struct std::is_placeholder<MyPlaceholder<N>> : public std::integral_constant<size_t, N> {};

namespace jasperengine {

    #ifdef JASPERENGINE_CRASH_GUARD
    using CrashHandler = std::function<void(const std::string& tag, const std::string& errorInfo)>;

    class FunctionSubscribeGlobal {
    public:
        static CrashHandler& getCrashHandler() {
            static CrashHandler handler = nullptr;
            return handler;
        }
    };
    #endif

    template<typename T>
    class FunctionSubscribe;

    template<typename Ret, typename ...Args>
    class FunctionSubscribe<Ret(Args...)> {

    private:
        // One subscription, shared by the copies of this object (a component array relocates
        // its components by copying them)
        struct Subscriber {
            std::string tag;
            std::function<Ret(Args...)> function;
            // the FunctionSubscribe objects that list it
            int owners = 0;
            // unsubscribed from all of them, or disabled by the crash guard: a running call skips it
            bool removed = false;
        };
        using SubscriberList = std::vector<std::shared_ptr<Subscriber>>;

        // Copied on write: a running call iterates the list it started with, so a callback can
        // subscribe, unsubscribe or destroy this object, and a call allocates nothing
        std::shared_ptr<SubscriberList> subscribers;
        bool enabled = true;

        void release(const std::shared_ptr<Subscriber>& subscriber) {
            if (--subscriber->owners == 0) {
                subscriber->removed = true;
            }
        }

        SubscriberList& editableSubscribers() {
            if (!subscribers) {
                subscribers = std::make_shared<SubscriberList>();
            } else if (subscribers.use_count() > 1) {
                subscribers = std::make_shared<SubscriberList>(*subscribers);
            }
            return *subscribers;
        }

        void removeAt(size_t index) {
            SubscriberList& list = editableSubscribers();
            release(list[index]);
            list.erase(list.begin() + index);
        }

        void copyFrom(const FunctionSubscribe& t) {
            if (t.subscribers) {
                for (const auto& subscriber : *t.subscribers) {
                    subscriber->owners++;
                }
            }
            clear();
            subscribers = t.subscribers;
            enabled = t.enabled;
        }

        template<typename T, size_t... Idx>
        std::function<Ret(Args...)> bindImpl(T *obj, Ret(T::*funcPtr)(Args...), std::index_sequence<Idx...>){
            return std::bind(funcPtr, obj, MyPlaceholder<Idx + 1>{}...);
        }

        #ifdef JASPERENGINE_CRASH_GUARD
        static std::string crashDescription(const CrashInfo& ci) {
            return std::string(ci.name ? ci.name : "UNKNOWN") +
                   " (" + (ci.description ? ci.description : "") +
                   ") [code=" + std::to_string(ci.code) + "]";
        }
        #endif

    public:

        FunctionSubscribe() = default;

        FunctionSubscribe(const FunctionSubscribe& t) {
            copyFrom(t);
        }

        ~FunctionSubscribe() {
            clear();
        }

        FunctionSubscribe& operator = (const FunctionSubscribe& t) {
            if (this != &t) {
                copyFrom(t);
            }

            return *this;
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
            if (subscribers) {
                for (const auto& subscriber : *subscribers) {
                    if (!subscriber->removed) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool add(const std::string& tag, lua_State *L){
            std::function<Ret(Args...)> function = LuaFunction<Ret>(L);
            return add(tag, std::move(function));
        }

        bool add(const std::string& tag, std::function<Ret(Args...)> function) {
            remove(tag);
            SubscriberList& list = editableSubscribers();
            // drops the ones the crash guard disabled
            list.erase(std::remove_if(list.begin(), list.end(),
                [](const std::shared_ptr<Subscriber>& subscriber) { return subscriber->removed; }), list.end());
            list.push_back(std::make_shared<Subscriber>(Subscriber{tag, std::move(function), 1}));
            return true;
        }

        template<Ret(*funcPtr)(Args...)>
        bool add(const std::string& tag){
            return add(tag, std::function<Ret(Args...)>(funcPtr));
        }

        template<typename T, Ret(T::*funcPtr)(Args...)>
        bool add(const std::string& tag, std::shared_ptr<T> obj){
            return add(tag, bindImpl(obj.get(), funcPtr, std::index_sequence_for<Args...>{}));
        }

        template<typename T, Ret(T::*funcPtr)(Args...)>
        bool add(const std::string& tag, T* obj){
            return add(tag, bindImpl(obj, funcPtr, std::index_sequence_for<Args...>{}));
        }

        template<typename T>
        bool add(const std::string& tag, std::shared_ptr<T> t){
            return add(tag, std::function<Ret(Args...)>(*t.get()));
        }

        bool remove(const std::string& tag){
            for (size_t i = 0; subscribers && i < subscribers->size(); i++) {
                if ((*subscribers)[i]->tag == tag) {
                    removeAt(i);
                    return true;
                }
            }

            return false;
        }

        size_t removeByTagSubstring(const std::string& substring){
            size_t removed = 0;
            for (size_t i = 0; subscribers && i < subscribers->size(); ) {
                if ((*subscribers)[i]->tag.find(substring) != std::string::npos) {
                    removeAt(i);
                    ++removed;
                } else {
                    ++i;
                }
            }
            return removed;
        }

        Ret call(Args... args){
            if (!enabled){
                if constexpr (!std::is_void<Ret>::value) {
                    return Ret();
                }
            }else{
                if constexpr (std::is_void<Ret>::value) {
                    // held: a callback can change or destroy this object
                    const std::shared_ptr<const SubscriberList> list = subscribers;
                    if (!list) {
                        return;
                    }
                    for (const auto& subscriber : *list) {
                        if (subscriber->removed) {
                            continue;
                        }
                        auto& function = subscriber->function;
                        #ifdef JASPERENGINE_CRASH_GUARD
                        // Use crash protection if handler is registered
                        auto& crashHandler = FunctionSubscribeGlobal::getCrashHandler();
                        if (crashHandler) {
                            CrashInfo ci{};
                            bool ok = callWithCrashGuard([&](){
                                function(args...);
                            }, &ci);

                            if (!ok) {
                                crashHandler(subscriber->tag, crashDescription(ci));

                                // not removeAt(): the callback may have destroyed this object
                                subscriber->removed = true;
                            }
                        } else {
                            function(args...);
                        }
                        #else
                        function(args...);
                        #endif
                    }
                } else {
                    return callRet(args..., Ret());
                }
            }
        }

        template<typename T>
        Ret callRet(Args... args, T def){
            if (!enabled){
                return def;
            }
            // held: a callback can change or destroy this object
            const std::shared_ptr<const SubscriberList> list = subscribers;
            if (!list) {
                return def;
            }
            for (const auto& subscriber : *list) {
                if (subscriber->removed) {
                    continue;
                }
                auto& function = subscriber->function;
                #ifdef JASPERENGINE_CRASH_GUARD
                // Use crash protection if handler is registered
                auto& crashHandler = FunctionSubscribeGlobal::getCrashHandler();
                if (crashHandler) {
                    Ret result{};
                    CrashInfo ci{};
                    bool ok = callWithCrashGuard([&](){
                        result = function(args...);
                    }, &ci);

                    if (ok) {
                        return result;
                    } else {
                        crashHandler(subscriber->tag, crashDescription(ci));

                        // not removeAt(): the callback may have destroyed this object
                        subscriber->removed = true;
                    }
                } else {
                    return function(args...);
                }
                #else
                return function(args...);
                #endif
            }
            return def;
        }

        Ret operator()(Args... args){
            return call(args...);
        }

        void clear(){
            if (subscribers) {
                for (const auto& subscriber : *subscribers) {
                    release(subscriber);
                }
                subscribers.reset();
            }
        }
    };

    class Scene;

    // True when a script instance may receive engine events: its GameObject is active
    // in hierarchy and its script entry is enabled (defined in ScriptBase.cpp).
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
