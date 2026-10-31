// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"
#include "component/ScriptComponent.h"
#include "PointerEventData.h"

namespace jasperengine {

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

        // Lifecycle (script-lifecycle spec 7.2). Override what the script needs.
        virtual void init() {}
        virtual void start() {}
        virtual void onEnable() {}
        virtual void onDisable() {}
        virtual void onDestroy() {}
        virtual void onApplicationPause(bool paused) { (void)paused; }
        virtual void onApplicationFocus(bool focused) { (void)focused; }
        virtual void onApplicationQuit() {}

        // The component UI's EventSystem callbacks (ui-multitouch, Unity's IPointerDownHandler...):
        // sent up the hierarchy from the element under the pointer to the first GameObject whose
        // script overrides them. The event lives for the call (copy what you keep); use() and
        // useDragThreshold write back.
        virtual void onPointerDown(PointerEventData& e) { (void)e; }
        virtual void onPointerUp(PointerEventData& e) { (void)e; }
        virtual void onPointerClick(PointerEventData& e) { (void)e; }
        virtual void onPointerEnter(PointerEventData& e) { (void)e; }
        virtual void onPointerExit(PointerEventData& e) { (void)e; }
        virtual void onInitializePotentialDrag(PointerEventData& e) { (void)e; }
        virtual void onBeginDrag(PointerEventData& e) { (void)e; }
        virtual void onDrag(PointerEventData& e) { (void)e; }
        virtual void onEndDrag(PointerEventData& e) { (void)e; }
        virtual void onDrop(PointerEventData& e) { (void)e; }
        virtual void onScroll(PointerEventData& e) { (void)e; }
    };

}

#endif //SCRIPT_BASE_H
