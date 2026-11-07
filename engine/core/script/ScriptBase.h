// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"
#include "ScriptProperty.h"
#include "ScriptScheduler.h"

#include <functional>
#include "component/ScriptComponent.h"
#include "PointerEventData.h"
#include "UIEventData.h"
#include "object/EntityHandle.h"

#include <map>
#include <string>
#include <type_traits>

namespace jasperengine {

    // What a C++ script keeps across a reload during Play (Preferences > Scripting > Reload
    // C++ Scripts During Play): values by name, and a blob for anything else. DPROPERTY
    // members are carried over without it; other members are lost unless saved here.
    struct ScriptReloadState {
        std::map<std::string, ScriptPropertyValue> values;
        std::string blob;
    };

    class Joint2D;
    class Joint3D;
    class Collision;
    class Collision2D;
    class ColliderRef;
    class ColliderRef2D;

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
        // virtual for the lifecycle callbacks; ends the async work owned by the script
        // (AsyncOwners::fromScript(this)) and its timers (invoke / invokeRepeating)
        virtual ~ScriptBase();

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

        // Unity's Invoke: calls `fn` once, `delay` seconds from now (scaled by Engine timeScale
        // unless TimeMode::Unscaled), in the update phase after onUpdate. The timer belongs to this
        // script: it keeps running while the script is disabled or its GameObject inactive, and is
        // cancelled when the script is deleted. Returns its id for cancelInvoke.
        TaskId invoke(std::function<void()> fn, float delay, TimeMode mode = TimeMode::Scaled);
        // Unity's InvokeRepeating: first after `delay`, then every `interval` (> 0) seconds, at most
        // once a frame (a late call is rescheduled from now)
        TaskId invokeRepeating(std::function<void()> fn, float delay, float interval, TimeMode mode = TimeMode::Scaled);
        // Cancels one of this script's timers; false when it is not one or already done
        bool cancelInvoke(TaskId id);
        // Cancels every timer of this script
        void cancelAllInvokes();
        // A timer of this script is pending (any, or the one with that id)
        bool isInvoking() const;
        bool isInvoking(TaskId id) const;

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

        // Unity's DontDestroyOnLoad(gameObject): this script's root GameObject (with its
        // children) survives scene loads in the persistent scene (SceneManager::
        // dontDestroyOnLoad). False, with a warning, when the GameObject has a parent.
        bool dontDestroyOnLoad();
        // In the persistent scene, or marked to move there
        bool isPersistent() const;

        // The singleton pattern of a persistent manager (Lua: GameObject.persistSingleton):
        //   if (persistSingleton<AdsManager>() != this) return;   // in init()
        // The instance of T already on a persistent GameObject (marked or moved), when there is
        // one other than this: this script's GameObject is destroyed (at the end of the frame)
        // and that one is returned. Otherwise this GameObject is made persistent and this is
        // returned (null when that is refused: the GameObject has a parent).
        template<class T>
        T* persistSingleton() {
            static_assert(std::is_base_of_v<ScriptBase, T>, "persistSingleton<T>: T is a C++ script class (ScriptBase)");
            if (T* other = static_cast<T*>(findPersistentScript([](ScriptBase* script) { return dynamic_cast<T*>(script) != nullptr; }))) {
                destroyOwnGameObject();
                return other;
            }
            return dontDestroyOnLoad() ? dynamic_cast<T*>(this) : nullptr;
        }

        // Engine-internal (persistence): the GameObject moved to another scene or entity id
        void rebind(Scene* scene, Entity entity);

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
        // A 3D joint of this GameObject (or whose bodyA is it) broke under `force` (N, Unity
        // OnJointBreak(float)); the joint still exists during the call
        virtual void onJointBreak(float force, Joint3D& joint);
        // Contacts (physics-contacts): on the main thread after the fixed step that caused them,
        // enabled or not, while the GameObject is active in hierarchy (docs/mcp/guide/physics.md).
        // A collision is seen from this side (its collider is the other one); a trigger gets the
        // other collider.
        virtual void onCollisionEnter(const Collision& collision);
        virtual void onCollisionStay(const Collision& collision);
        virtual void onCollisionExit(const Collision& collision);
        virtual void onTriggerEnter(const ColliderRef& other);
        virtual void onTriggerStay(const ColliderRef& other);
        virtual void onTriggerExit(const ColliderRef& other);
        virtual void onCollisionEnter2D(const Collision2D& collision);
        virtual void onCollisionStay2D(const Collision2D& collision);
        virtual void onCollisionExit2D(const Collision2D& collision);
        virtual void onTriggerEnter2D(const ColliderRef2D& other);
        virtual void onTriggerStay2D(const ColliderRef2D& other);
        virtual void onTriggerExit2D(const ColliderRef2D& other);

        // A Particle System of this GameObject with main.stopAction Callback finished: it stopped
        // and its last particle died (Unity OnParticleSystemStopped).
        virtual void onParticleSystemStopped();

        // The component UI's EventSystem callbacks (ui-multitouch, Unity's IPointerDownHandler...):
        // sent up the hierarchy from the element under the pointer to the first GameObject whose
        // script overrides them. The event lives for the call (copy what you keep); use() and
        // useDragThreshold write back.
        virtual void onPointerDown(PointerEventData& e);
        virtual void onPointerUp(PointerEventData& e);
        virtual void onPointerClick(PointerEventData& e);
        virtual void onPointerEnter(PointerEventData& e);
        virtual void onPointerExit(PointerEventData& e);
        virtual void onInitializePotentialDrag(PointerEventData& e);
        virtual void onBeginDrag(PointerEventData& e);
        virtual void onDrag(PointerEventData& e);
        virtual void onEndDrag(PointerEventData& e);
        virtual void onDrop(PointerEventData& e);
        virtual void onScroll(PointerEventData& e);

        // The component UI's selection callbacks (ui-components, Unity's ISelectHandler,
        // IDeselectHandler, IMoveHandler, ISubmitHandler, ICancelHandler): onSelect, onDeselect,
        // onMove and onSubmit go to the selected GameObject, onCancel up its hierarchy to the
        // first that handles it. use() in onMove and onSubmit skips the Button's own navigation
        // and click.
        virtual void onSelect(BaseEventData& e);
        virtual void onDeselect(BaseEventData& e);
        virtual void onMove(AxisEventData& e);
        virtual void onSubmit(BaseEventData& e);
        virtual void onCancel(BaseEventData& e);

        // the messages (ScriptMessage bits) whose base method ran: not overridden, so not sent
        // again and not among ScriptMessages::receivers
        uint64_t inheritedMessages = 0;   // ScriptMessageMask
        // set by a base message method (ScriptMessages reads it after a call without touching a
        // script the call may have destroyed)
        static bool baseMessageRan;

    private:
        // A live C++ script on a persistent GameObject (or one queued to move there) other
        // than this one, for which `matches` is true
        ScriptBase* findPersistentScript(bool (*matches)(ScriptBase*)) const;
        void destroyOwnGameObject();
    };

    // Engine-internal, for the generated code of C++ scripts (persistence): a script object or
    // a wrapper a script property owns follows a GameObject that moved to another scene
    template<class T>
    void rebindScriptObject(T* object, Scene* scene, Entity entity) {
        if (!object) {
            return;
        }
        if constexpr (std::is_base_of_v<ScriptBase, T>) {
            static_cast<ScriptBase*>(object)->rebind(scene, entity);
        }
        if constexpr (std::is_base_of_v<EntityHandle, T>) {
            static_cast<EntityHandle*>(object)->rebind(scene, entity);
        }
    }

    // ...when it points at one of the moved GameObjects (`oldEntities[i]` of `from`, now
    // `newEntities[i]` of `to`)
    template<class T>
    void rebindScriptReference(T* object, Scene* from, Scene* to, const Entity* oldEntities, const Entity* newEntities, size_t count) {
        if constexpr (std::is_base_of_v<EntityHandle, T> || std::is_base_of_v<ScriptBase, T>) {
            if (!object) {
                return;
            }
            Scene* scene = nullptr;
            Entity entity = NULL_ENTITY;
            if constexpr (std::is_base_of_v<EntityHandle, T>) {
                scene = static_cast<EntityHandle*>(object)->getScene();
                entity = static_cast<EntityHandle*>(object)->getEntity();
            } else {
                scene = static_cast<ScriptBase*>(object)->getScene();
                entity = static_cast<ScriptBase*>(object)->getEntity();
            }
            if (scene != from) {
                return;
            }
            for (size_t i = 0; i < count; i++) {
                if (oldEntities[i] == entity) {
                    rebindScriptObject(object, to, newEntities[i]);
                    return;
                }
            }
        } else {
            (void)object; (void)from; (void)to; (void)oldEntities; (void)newEntities; (void)count;
        }
    }

}

#endif //SCRIPT_BASE_H
