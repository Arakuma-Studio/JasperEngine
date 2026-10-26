// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaFunction.h"

#include "math/Vector2.h"
#include "util/FunctionSubscribe.h"
#include "util/CrashHook.h"
#include "util/StringUtils.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "object/physics/Contact2D.h"
#include "object/physics/Manifold2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/Contact3D.h"
#include "object/physics/CollideShapeResult3D.h"
#endif
#include "LuaBinding.h"
#include "LuaReload.h"
#include "Log.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include <stdexcept>

using namespace jasperengine;

LuaFunctionBase::LuaFunctionBase(lua_State *vm, const std::string &func): m_vm(vm) {
    // get the function
    lua_getglobal(m_vm, func.c_str());

    store_function();
}

LuaFunctionBase::LuaFunctionBase(lua_State *vm): m_vm(vm) {
    store_function();
}

LuaFunctionBase::LuaFunctionBase(const LuaFunctionBase &other): m_vm(other.m_vm), m_record(other.m_record) {
    // copy the registry reference
    lua_rawgeti(m_vm, LUA_REGISTRYINDEX, other.m_func);
    m_func = luaL_ref(m_vm, LUA_REGISTRYINDEX);
}

LuaFunctionBase::~LuaFunctionBase(){
    // delete the reference from registry
    if (LuaBinding::getLuaState() && m_func != LUA_NOREF){ //check if state is not closed
        luaL_unref(m_vm, LUA_REGISTRYINDEX, m_func);
    }
}

LuaFunctionBase& LuaFunctionBase::operator=(const LuaFunctionBase &other){
    if (this != &other) {
        m_vm = other.m_vm;
        m_record = other.m_record;
        lua_rawgeti(m_vm, LUA_REGISTRYINDEX, other.m_func);
        m_func = luaL_ref(m_vm, LUA_REGISTRYINDEX);
    }
    return *this;
}

void LuaFunctionBase::store_function(){
    // ensure it's a function
    if (!lua_isfunction(m_vm, -1)) {
        lua_pop(m_vm, 1);
        throw std::runtime_error("not a valid function");
    }
    m_record = lua_reload::callbackRecordForFunction(m_vm);
    // store it in registry for later use
    m_func = luaL_ref(m_vm, LUA_REGISTRYINDEX);
}

void LuaFunctionBase::push_function(lua_State *vm, int func){
    lua_rawgeti(vm, LUA_REGISTRYINDEX, func);
}

void LuaFunctionBase::call(int args, int results) {
    if (m_record && m_record->dropped()) {
        // an anonymous callback of code a hot reload replaced
        lua_pop(m_vm, args + 1);
        for (int i = 0; i < results; i++) {
            lua_pushnil(m_vm);
        }
        return;
    }
    int status = LuaBinding::pcallWithTraceback(m_vm, args, results);
    if (status) {
        std::string error = LuaBinding::getLuaStackErrorString(m_vm, -1);
        lua_pop(m_vm, 1);

        // while Play runs the editor's crash hook gets it (pause, then stop)
        if (!CrashHook::report("Lua Script Error", error)) {
            LuaBinding::logLuaError("Lua Error: %s", error.c_str());
        }

        // Push nil values to satisfy the expected results count
        for (int i = 0; i < results; i++) {
            lua_pushnil(m_vm);
        }
    }
}

void LuaFunctionBase::push_value(lua_State *vm, int n){ 
    lua_pushinteger(vm, n); 
}

void LuaFunctionBase::push_value(lua_State *vm, unsigned int n){ 
    lua_pushinteger(vm, n); 
}

void LuaFunctionBase::push_value(lua_State *vm, float n){
    lua_pushnumber(vm, n); 
}

void LuaFunctionBase::push_value(lua_State *vm, long n){
    lua_pushnumber(vm, n); 
}

void LuaFunctionBase::push_value(lua_State *vm, unsigned long n){
    lua_pushnumber(vm, n); 
}

void LuaFunctionBase::push_value(lua_State *vm, bool b){
    lua_pushboolean(vm, b);
}

void LuaFunctionBase::push_value(lua_State *vm, const std::string &s){
    lua_pushstring(vm, s.c_str());
}

void LuaFunctionBase::push_value(lua_State *vm, wchar_t s){
    lua_pushstring(vm, StringUtils::toUTF8(s).c_str());
}

void LuaFunctionBase::push_value(lua_State *vm, Vector2 o){
    if (!luabridge::push<Vector2>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Vector3 o){
    if (!luabridge::push<Vector3>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

#ifdef JASPERENGINE_PHYSICS_2D
void LuaFunctionBase::push_value(lua_State *vm, Rigidbody2D o){
    if (!luabridge::push<Rigidbody2D>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Manifold2D o){
    if (!luabridge::push<Manifold2D>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Contact2D o){
    if (!luabridge::push<Contact2D>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void LuaFunctionBase::push_value(lua_State *vm, Rigidbody o){
    if (!luabridge::push<Rigidbody>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Contact3D o){
    if (!luabridge::push<Contact3D>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, CollideShapeResult3D o){
    if (!luabridge::push<CollideShapeResult3D>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}
#endif

template <>
int LuaFunctionBase::get_value<int>(lua_State* vm){
    int val = lua_tointeger(vm, -1);
    lua_pop(vm, 1);
    return val;
}

template <>
float LuaFunctionBase::get_value<float>(lua_State* vm){
    float val = lua_tonumber(vm, -1);
    lua_pop(vm, 1);
    return val;
}

template <>
bool LuaFunctionBase::get_value<bool>(lua_State* vm){
    bool val = lua_toboolean(vm, -1);
    lua_pop(vm, 1);
    return val;
}

template <>
std::string LuaFunctionBase::get_value<std::string>(lua_State* vm){
    std::string val = lua_tostring(vm, -1);
    lua_pop(vm, 1);
    return val;
}
