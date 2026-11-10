// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_RUNTIME_H
#define SCRIPT_RUNTIME_H

#include "Export.h"
#include "ecs/Entity.h"
#include "ScriptMessages.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    class Scene;
    struct ScriptEntry;

    // The lifecycle state of one script instance (script-lifecycle spec 6.1), kept by
    // ScriptRuntime for the instance (a Lua instance's registry ref or a C++ object): copies of
    // the Script component's entries (undo, reordering) cannot disturb it
    struct JASPERENGINE_API ScriptState {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        bool initialized = false;    // init (Awake) ran
        bool enabledLatch = false;   // onEnable ran and onDisable has not since
        bool started = false;        // start ran
        bool startQueued = false;    // waiting in the start queue
        bool destroying = false;     // being destroyed or unloaded: it is never enabled again
        bool destroyed = false;      // onDestroy ran (or was not due): no more messages
    };

    // Unity's MonoBehaviour lifecycle for Lua and C++ scripts (script-lifecycle spec 6), on the
    // ScriptMessages mechanism. Every pass goes over scenes in Engine order, GameObjects in
    // hierarchy pre-order (EntityRegistry::getHierarchyOrder), scripts in component order.
    //
    //   init        once, when the GameObject is first active in hierarchy (enabled or not)
    //   onEnable    when it becomes active in hierarchy and enabled (after init)
    //   start       once, at the frame start point after the first onEnable (flushStarts)
    //   onDisable   when it stops being active and enabled (before onDestroy)
    //   onDestroy   once, on destroy, scene unload or quit, only if init ran
    //
    // onEnable / onDisable come from one latch per script (refresh), so they always pair, also
    // when a callback activates, deactivates, enables or destroys something itself.
    class JASPERENGINE_API ScriptRuntime {
    public:
        // A new scene's scripts, their instances made and bound: all init (pre-order), then all
        // onEnable (pre-order). From here on the scene "has started": activation and enabling
        // changes call the callbacks.
        static void awakeScene(Scene* scene);
        // GameObjects instantiated into a started scene (their instances made and bound)
        static void awakeEntities(Scene* scene, const std::vector<Entity>& entities);
        // EntityRegistry::setEntityActive / addEntityChild: the GameObjects whose
        // activeInHierarchy flipped (pre-order)
        static void onActiveInHierarchyChanged(Scene* scene, const std::vector<Entity>& changed, bool nowActive);
        // Unity's enabled = x: sets the entry's flag, then onEnable / onDisable as due
        static void setScriptEnabled(Scene* scene, Entity entity, size_t index, bool enabled);
        // After the entries' enabled flags were written directly (the editor's commands, undo):
        // onEnable / onDisable as due. Nothing before the scene has started.
        static void refreshEntity(Scene* scene, Entity entity);
        // The frame start point (Engine::runFrame, before the fixed updates): start of every
        // script queued until now; scripts queued meanwhile wait for the next frame
        static void flushStarts();
        // A private editor preview starts its own scripts without consuming another scene's
        // queued starts. Null keeps the normal frame-wide behavior.
        static void flushStarts(Scene* onlyScene);
        // Destroy: onDisable (pre-order), then onDestroy, before their instances are released
        static void destroyEntities(Scene* scene, const std::vector<Entity>& entities);
        // Scene unload: the same over whole scenes (all onDisable, then all onDestroy); the
        // scenes have not started any more. Entries already destroyed are skipped.
        static void unloadScenes(const std::vector<Scene*>& scenes);
        // Play stop / application quit: onApplicationQuit to every live instance of every
        // started scene, then unloadScenes of them all
        static void quitAll();
        // Every running scene (Engine order; then the other started scenes), GameObjects active
        // in hierarchy, live instances enabled or not (application pause and focus)
        static void broadcast(ScriptMessage m, const std::function<int(lua_State*)>& pushArgs,
                              const std::function<void(ScriptBase&)>& callCpp);
        // ScriptMessages::send with delivery rules (DeliverRequireActive, DeliverRequireEnabled)
        static int sendMessage(Scene* scene, Entity entity, ScriptMessage m,
                               const std::function<int(lua_State*)>& pushArgs,
                               const std::function<void(ScriptBase&)>& callCpp,
                               uint32_t deliver = DeliverAny);

        // Unity's ExecuteEvents.GetEventHandler: the GameObject itself or its first ancestor (upward)
        // that is active in hierarchy and has an enabled script defining the message
        // (ScriptMessages::defines); NULL_ENTITY when none
        static Entity findHandler(Scene* scene, Entity entity, ScriptMessage m);

        // The scene's scripts were awakened and it is not unloaded
        static bool hasStarted(Scene* scene);
        // The started scenes: the running ones in Engine order, then the others in start order
        static std::vector<Scene*> getStartedScenes();
        // The scene is being deleted: forget it (its queued starts too)
        static void forgetScene(Scene* scene);
        // GameObjects moved to another scene (persistence): their queued starts follow
        static void remap(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& entities);

        // The entry of a script instance (a Lua instance's ref or a C++ instance), with its index
        static ScriptEntry* findEntry(Scene* scene, Entity entity, const void* instance, size_t* index = nullptr);
        // The lifecycle state of an instance; null before the runtime saw it
        static const ScriptState* getState(const void* instance);
        // The instance is released or deleted (its ref or address may be reused): its state goes
        static void forgetInstance(const void* instance);
        // What a released script (`tag`: the "_<address>_" its subscriptions carry) subscribed
        // goes: the Engine's events, its scene's component events, and the persistent scene's
        // (a level's script listening to a persistent GameObject; DontDestroyOnLoad)
        static void removeSubscriptionsByTag(Scene* scene, const std::string& tag);
    };

}

#endif // SCRIPT_RUNTIME_H
