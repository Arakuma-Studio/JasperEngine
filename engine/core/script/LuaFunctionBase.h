// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUA_FUNCTIONBASE_H
#define LUA_FUNCTIONBASE_H

#include <memory>
#include <string>
#include <vector>
#include "math/Vector3.h"
#include "math/Quaternion.h"

typedef struct lua_State lua_State;

namespace jasperengine{

    class Rigidbody2D;
    class Manifold2D;
    class Contact2D;
    class Rigidbody;
    class Contact3D;
    class CollideShapeResult3D;
    struct ControllerColliderHit;
    class NavMeshAgent;
    class NavMeshSurface;
    class InputActionEvent;
    struct ParticleCollisionInfo;
    struct ParticleDeathInfo;
    struct ParticleSubEmitInfo;
    struct AnimationEvent;
    struct VoxelEditEvent;
    class AABB;
    class GameObject;
    class VideoPlayer;

    namespace lua_reload { struct CallbackRecord; }

    // the base function wrapper class
    class LuaFunctionBase{

    private:

        // takes the function on top of vm; keeps the main thread of vm's state
        void store_function(lua_State *vm);

    protected:
        // the virtual machine and the registry reference to the function
        lua_State *m_vm;
        int m_func;
        // the generation of the chunk that defined the function: once a hot reload retired
        // it (the function is the old code's), calls do nothing (LuaReload)
        std::shared_ptr<lua_reload::CallbackRecord> m_record;
        // the Lua state the function belongs to (LuaBinding::getLuaStateGeneration): a function
        // of a closed state (the engine shut down; an Android activity relaunched in the same
        // process starts a new one) is never called nor released. It may be a coroutine's
        // lua_State of that state, which shares its registry.
        uint64_t m_generation = 0;

        // push the function from the registry
        void push_function(lua_State *vm, int func);

        // call the function, logs a traceback on error
        void call(int args, int results = 0);

        // we overload push_value instead of specializing
        // because this way we can also push values that
        // are implicitly convertible to one of the types

        void push_value(lua_State *vm, int n);
        void push_value(lua_State *vm, unsigned int n);
        void push_value(lua_State *vm, float n);
        void push_value(lua_State *vm, long n);
        void push_value(lua_State *vm, unsigned long n);
        void push_value(lua_State *vm, bool b);
        void push_value(lua_State *vm, const std::string &s);
        // a typed character (Engine.onCharInput): its UTF-8 string
        void push_value(lua_State *vm, char32_t s);
        void push_value(lua_State *vm, Vector2 o);
        void push_value(lua_State *vm, Vector3 o);
        // the Animator's onAnimatorMove (delta position, delta rotation)
        void push_value(lua_State *vm, Quaternion o);
        // a particle system's batched events: an array of plain tables
        void push_value(lua_State *vm, const std::vector<ParticleCollisionInfo>& events);
        void push_value(lua_State *vm, const std::vector<ParticleDeathInfo>& events);
        void push_value(lua_State *vm, const std::vector<ParticleSubEmitInfo>& events);
        void push_value(lua_State *vm, Rigidbody2D o);
        void push_value(lua_State *vm, Contact2D o);
        void push_value(lua_State *vm, Manifold2D o);
        void push_value(lua_State *vm, Rigidbody o);
        void push_value(lua_State *vm, Contact3D o);
        void push_value(lua_State *vm, CollideShapeResult3D o);
        // a copy of the event (its args read as a 1-based table: AnimationEvent.args)
        void push_value(lua_State *vm, const AnimationEvent &o);
        void push_value(lua_State *vm, ControllerColliderHit o);
        void push_value(lua_State *vm, NavMeshAgent o);
        void push_value(lua_State *vm, NavMeshSurface o);
        // an input action's callback gets the event itself: event:consume() reaches it
        void push_value(lua_State *vm, const InputActionEvent& o);
        // a Voxel Volume's events (T6 R31): onEdited's plain table, onCollisionUpdated's bounds, onBodyRescued's GameObject
        void push_value(lua_State *vm, const VoxelEditEvent& o);
        void push_value(lua_State *vm, const AABB& o);
        void push_value(lua_State *vm, const GameObject& o);
        // the Video Player of an event (onPrepareCompleted, ...)
        void push_value(lua_State *vm, const VideoPlayer &o);

        template <typename T>
        static T get_value(lua_State *vm);

    public:
        // its Lua state still exists: calls run and the destructor releases its reference
        bool isLive() const;

        LuaFunctionBase(lua_State *vm, const std::string &func);
        LuaFunctionBase(lua_State *vm);

        LuaFunctionBase(const LuaFunctionBase &other);

        ~LuaFunctionBase();

        LuaFunctionBase &operator=(const LuaFunctionBase &other);
    };

}

#endif // LUA_FUNCTIONBASE_H