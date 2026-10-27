// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUA_FUNCTIONBASE_H
#define LUA_FUNCTIONBASE_H

#include <memory>
#include <string>
#include <vector>
#include "math/Vector3.h"

typedef struct lua_State lua_State;

namespace jasperengine{

    class Rigidbody2D;
    class Manifold2D;
    class Contact2D;
    class Rigidbody;
    class Contact3D;
    class CollideShapeResult3D;
    struct ParticleCollisionInfo;
    struct ParticleDeathInfo;
    struct ParticleSubEmitInfo;
    struct AnimationEvent;

    namespace lua_reload { struct CallbackRecord; }

    // the base function wrapper class
    class LuaFunctionBase{

    private:

        void store_function();

    protected:
        // the virtual machine and the registry reference to the function
        lua_State *m_vm;
        int m_func;
        // the generation of the chunk that defined the function: once a hot reload retired
        // it (the function is the old code's), calls do nothing (LuaReload)
        std::shared_ptr<lua_reload::CallbackRecord> m_record;

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
        void push_value(lua_State *vm, wchar_t s);
        void push_value(lua_State *vm, Vector2 o);
        void push_value(lua_State *vm, Vector3 o);
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

        template <typename T>
        static T get_value(lua_State *vm);

    public:
        LuaFunctionBase(lua_State *vm, const std::string &func);
        LuaFunctionBase(lua_State *vm);

        LuaFunctionBase(const LuaFunctionBase &other);

        ~LuaFunctionBase();

        LuaFunctionBase &operator=(const LuaFunctionBase &other);
    };

}

#endif // LUA_FUNCTIONBASE_H