// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ASYNCOWNER_H
#define ASYNCOWNER_H

#include "Export.h"
#include "ecs/Entity.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>
#include <string>

typedef struct lua_State lua_State;

namespace jasperengine {

    class Scene;
    class EntityRegistry;
    class ScriptBase;

    // Who an asynchronous piece of work (an HTTP request, a native call, a timer) belongs to.
    // Work with an owner ends with it, silently (no callback runs): a script instance when it
    // is released (destroyed, its scene unloaded, Play stopped), a GameObject when it is
    // destroyed, a Scene when it is deleted. Without an owner the work is global and survives
    // scene loads. One owner mechanism for the whole engine: http, native-bridge and
    // coroutines use these tokens.
    struct AsyncOwner {
        uint64_t token = 0;     // 0: no owner
        bool isNone() const { return token == 0; }
    };

    class JASPERENGINE_API AsyncOwners {
    public:
        // `self` of a Lua script (a script instance table), a GameObject or any EntityHandle,
        // a Scene; nil/none -> no owner. Anything else: false with `error` ("owner must be a
        // script instance (self), a GameObject or a Scene"). The main thread only.
        static bool fromLua(lua_State* L, int index, AsyncOwner& out, std::string& error);

        // A GameObject; an entity that does not exist gives an owner that is already gone
        static AsyncOwner fromEntity(Scene* scene, Entity entity);
        // A Scene: the work ends when the scene is deleted
        static AsyncOwner fromScene(Scene* scene);
        // A C++ script (`AsyncOwners::fromScript(this)`): ends with the script object and with
        // its GameObject
        static AsyncOwner fromScript(const ScriptBase* script);
        // A script instance by the key the engine knows it by (a Lua script: the registry ref
        // of its instance table as a pointer, ScriptEntry::instance; a C++ script: its
        // ScriptBase*); `scene`/`entity`, when given, end it with its GameObject too
        static AsyncOwner fromScriptInstance(const void* instance, Scene* scene = nullptr, Entity entity = NULL_ENTITY);

        // Still there (no owner: always)
        static bool alive(const AsyncOwner& owner);
        // "script Player (GameObject 'Hero')", "GameObject 'Hero'", "scene 'Level1'", "none"
        static std::string describe(const AsyncOwner& owner);

        // Engine hooks (each ends the matching owners and fires onOwnerGone for each)
        static void scriptInstanceReleased(const void* instance);   // LuaBinding::releaseLuaScriptInstance, ~ScriptBase
        static void sceneDestroyed(const EntityRegistry* scene);    // Scene::~Scene (also ends its GameObjects' owners)
        static void entityDestroyed(const EntityRegistry* scene, Entity entity);   // EntityRegistry::destroyEntity(ies)

        // Fired with the token of an owner that is gone, on the thread that ended it (the main
        // thread in practice); subscribers must not create or end owners from inside
        static FunctionSubscribe<void(uint64_t token)>& onOwnerGone();

        // Owners registered now (tests, stats)
        static size_t count();
    };

}

#endif // ASYNCOWNER_H
