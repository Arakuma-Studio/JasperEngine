// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUA_FUNCTION_H
#define LUA_FUNCTION_H

// Based on: http://stackoverflow.com/questions/7885299/c-call-lua-function-with-variable-parameters

#include "LuaFunctionBase.h"
#include <string>
#include <type_traits>

typedef struct lua_State lua_State;

namespace jasperengine{

    template <typename Ret>
    class LuaFunction : public LuaFunctionBase {

    public:
        LuaFunction(lua_State *vm, const std::string &func)
        : LuaFunctionBase(vm, func){ }

        LuaFunction(lua_State *vm)
        : LuaFunctionBase(vm){ }

        Ret operator()() {
            if (!isLive()) return Ret{};
            push_function(m_vm, m_func);
            call(0, 1);
            return get_value<Ret>(m_vm);
        }

        template <typename... Args, std::enable_if_t<(sizeof...(Args) > 0 && sizeof...(Args) <= 6), int> = 0>
        Ret operator()(const Args&... args){
            if (!isLive()) return Ret{};
            push_function(m_vm, m_func);
            (push_value(m_vm, args), ...);
            call(sizeof...(Args), 1);
            return get_value<Ret>(m_vm);
        }

    };

    // we need to specialize the function for void return type
    // as the other class would fail to compile with void as return type
    template <>
    class LuaFunction<void> : public LuaFunctionBase {

    public:
        LuaFunction(lua_State *vm, const std::string &func)
        : LuaFunctionBase(vm, func) { }

        LuaFunction(lua_State *vm)
        : LuaFunctionBase(vm) { }

        void operator()(){
            if (!isLive()) return;
            push_function(m_vm, m_func);
            call(0);
        }

        template <typename... Args, std::enable_if_t<(sizeof...(Args) > 0 && sizeof...(Args) <= 8), int> = 0>
        void operator()(const Args&... args){
            if (!isLive()) return;
            push_function(m_vm, m_func);
            (push_value(m_vm, args), ...);
            call(sizeof...(Args));
        }

    };

}

#endif // LUA_FUNCTION_H