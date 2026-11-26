// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SceneConstructors_h
#define SceneConstructors_h

// The Lua constructors of the objects that live in a scene (GameObject and its kinds, the actions,
// CullingGroup): GameObject(scene), GameObject(scene, entity). A nil scene raises a Lua error
// naming the fix. main.lua runs before the game's init sets a scene, so Engine.getScene() is nil
// there; made with it, an object wrote its entity through a null Scene: a crash on desktop and,
// on the web (where address 0 is ordinary memory), a page that hung before its first frame.
//
// lua_State* comes last: LuaBridge counts it as a stack slot of its own, so before the scene it
// would shift the scene off its argument. The error leaves before the object is constructed, so
// the userdata stays uncommitted and nothing is destroyed for it.

#include "ecs/Entity.h"
#include "lua.hpp"

#include <new>

namespace jasperengine {

    class Scene;

    namespace sceneconstructors {

        inline void requireScene(const Scene* scene, lua_State* L){
            if (!scene)
                luaL_error(L, "the scene is nil: a GameObject needs one. Engine.getScene() is nil until the game's init "
                              "sets a scene (main.lua runs before it): make objects in init, a script's init or later");
        }

        // T(scene)
        template <class T>
        auto ofScene(){
            return [](void* place, Scene* scene, lua_State* L) -> T* {
                requireScene(scene, L);
                return new (place) T(scene);
            };
        }

        // T(scene, entity): an object over an entity the scene has
        template <class T>
        auto ofEntity(){
            return [](void* place, Scene* scene, Entity entity, lua_State* L) -> T* {
                requireScene(scene, L);
                return new (place) T(scene, entity);
            };
        }

        // T(scene, value): the constructors with one more argument (AudioSource(scene, is3D))
        template <class T, class A>
        auto ofSceneWith(){
            return [](void* place, Scene* scene, A value, lua_State* L) -> T* {
                requireScene(scene, L);
                return new (place) T(scene, value);
            };
        }

    }

}

#endif
