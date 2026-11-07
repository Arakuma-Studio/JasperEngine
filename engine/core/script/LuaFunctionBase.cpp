// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaFunction.h"
#include "object/navigation/NavMeshAgent.h"
#include "object/navigation/NavMeshSurface.h"

#include "math/Vector2.h"
#include "util/FunctionSubscribe.h"
#include "util/CrashHook.h"
#include "util/StringUtils.h"
#include "component/ParticleSystemComponent.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "object/physics/Contact2D.h"
#include "object/physics/Manifold2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "object/physics/Contact3D.h"
#include "object/physics/CollideShapeResult3D.h"
#include "object/physics/ControllerColliderHit.h"
#endif
#include "LuaBinding.h"
#include "input/InputActions.h"
#include "LuaReload.h"
#include "LuaWatchdog.h"
#include "Log.h"
#include "animation/AnimationEvent.h"
#include "object/VideoPlayer.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include <stdexcept>

using namespace jasperengine;

LuaFunctionBase::LuaFunctionBase(lua_State *vm, const std::string &func): m_vm(vm), m_func(LUA_NOREF), m_generation(LuaBinding::getLuaStateGeneration()) {
    // get the function
    lua_getglobal(vm, func.c_str());

    store_function(vm);
}

LuaFunctionBase::LuaFunctionBase(lua_State *vm): m_vm(vm), m_func(LUA_NOREF), m_generation(LuaBinding::getLuaStateGeneration()) {
    store_function(vm);
}

LuaFunctionBase::LuaFunctionBase(const LuaFunctionBase &other)
    : m_vm(other.m_vm), m_func(LUA_NOREF), m_record(other.m_record), m_generation(other.m_generation) {
    // copy the registry reference (a closed state's function stays a dead copy)
    if (other.isLive() && other.m_func != LUA_NOREF) {
        lua_rawgeti(m_vm, LUA_REGISTRYINDEX, other.m_func);
        m_func = luaL_ref(m_vm, LUA_REGISTRYINDEX);
    }
}

LuaFunctionBase::~LuaFunctionBase(){
    // delete the reference from the registry, of its own state only (a closed one freed it)
    if (isLive() && m_func != LUA_NOREF){
        luaL_unref(m_vm, LUA_REGISTRYINDEX, m_func);
    }
}

bool LuaFunctionBase::isLive() const {
    return m_vm && LuaBinding::getLuaState() && m_generation == LuaBinding::getLuaStateGeneration();
}

LuaFunctionBase& LuaFunctionBase::operator=(const LuaFunctionBase &other){
    if (this != &other) {
        if (isLive() && m_func != LUA_NOREF) luaL_unref(m_vm, LUA_REGISTRYINDEX, m_func);
        m_vm = other.m_vm;
        m_record = other.m_record;
        m_generation = other.m_generation;
        m_func = LUA_NOREF;
        if (other.isLive() && other.m_func != LUA_NOREF) {
            lua_rawgeti(m_vm, LUA_REGISTRYINDEX, other.m_func);
            m_func = luaL_ref(m_vm, LUA_REGISTRYINDEX);
        }
    }
    return *this;
}

void LuaFunctionBase::store_function(lua_State *vm){
    // ensure it's a function
    if (!lua_isfunction(vm, -1)) {
        lua_pop(vm, 1);
        throw std::runtime_error("not a valid function");
    }
    m_record = lua_reload::callbackRecordForFunction(m_vm);
    // store it in registry for later use
    m_func = luaL_ref(vm, LUA_REGISTRYINDEX);
    // called later from the main thread: `vm` may be a coroutine's, which ends (and is
    // collected) long before the callback runs
    m_vm = LuaBinding::mainThread(vm);
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

        // the hang watchdog interrupted a runaway function: a script crash, Play goes on;
        // otherwise, while Play runs, the editor's crash hook gets it (pause, then stop)
        if (LuaWatchdog::takeInterrupt()) {
            LuaBinding::logLuaError("Script crash: %s", error.c_str());
        } else if (!CrashHook::report("Lua Script Error", error)) {
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

void LuaFunctionBase::push_value(lua_State *vm, char32_t s){
    lua_pushstring(vm, StringUtils::toUTF8(s).c_str());
}

void LuaFunctionBase::push_value(lua_State *vm, const AnimationEvent &o){
    if (!luabridge::push<AnimationEvent>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Vector2 o){
    if (!luabridge::push<Vector2>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, const InputActionEvent& o){
    if (!luabridge::push<InputActionEvent*>(vm, const_cast<InputActionEvent*>(&o)))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Vector3 o){
    if (!luabridge::push<Vector3>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, Quaternion o){
    if (!luabridge::push<Quaternion>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

// {position = Vector3, normal = Vector3, velocity = Vector3, particleId = n, killed = b}, ...
void LuaFunctionBase::push_value(lua_State *vm, const std::vector<ParticleCollisionInfo>& events){
    lua_createtable(vm, (int)events.size(), 0);
    for (size_t i = 0; i < events.size(); i++){
        const ParticleCollisionInfo& e = events[i];
        lua_createtable(vm, 0, 5);
        push_value(vm, e.position);
        lua_setfield(vm, -2, "position");
        push_value(vm, e.normal);
        lua_setfield(vm, -2, "normal");
        push_value(vm, e.velocity);
        lua_setfield(vm, -2, "velocity");
        lua_pushinteger(vm, (lua_Integer)e.particleId);
        lua_setfield(vm, -2, "particleId");
        lua_pushboolean(vm, e.killed);
        lua_setfield(vm, -2, "killed");
        lua_rawseti(vm, -2, (lua_Integer)i + 1);
    }
}

void LuaFunctionBase::push_value(lua_State *vm, const std::vector<ParticleDeathInfo>& events){
    lua_createtable(vm, (int)events.size(), 0);
    for (size_t i = 0; i < events.size(); i++){
        const ParticleDeathInfo& e = events[i];
        lua_createtable(vm, 0, 3);
        push_value(vm, e.position);
        lua_setfield(vm, -2, "position");
        push_value(vm, e.velocity);
        lua_setfield(vm, -2, "velocity");
        lua_pushinteger(vm, (lua_Integer)e.particleId);
        lua_setfield(vm, -2, "particleId");
        lua_rawseti(vm, -2, (lua_Integer)i + 1);
    }
}

void LuaFunctionBase::push_value(lua_State *vm, const std::vector<ParticleSubEmitInfo>& events){
    lua_createtable(vm, (int)events.size(), 0);
    for (size_t i = 0; i < events.size(); i++){
        const ParticleSubEmitInfo& e = events[i];
        lua_createtable(vm, 0, 4);
        push_value(vm, e.position);
        lua_setfield(vm, -2, "position");
        // the entry counted from 1, as Lua lists are
        lua_pushinteger(vm, (lua_Integer)e.entry + 1);
        lua_setfield(vm, -2, "entry");
        lua_pushinteger(vm, (lua_Integer)e.count);
        lua_setfield(vm, -2, "count");
        lua_pushstring(vm, e.trigger.c_str());
        lua_setfield(vm, -2, "trigger");
        lua_rawseti(vm, -2, (lua_Integer)i + 1);
    }
}

void LuaFunctionBase::push_value(lua_State *vm, NavMeshAgent o){
    if (!luabridge::push<NavMeshAgent>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, NavMeshSurface o){
    if (!luabridge::push<NavMeshSurface>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

void LuaFunctionBase::push_value(lua_State *vm, nav::PathFailure o){
    lua_pushstring(vm, nav::pathFailureName(o));
}

void LuaFunctionBase::push_value(lua_State *vm, const VideoPlayer &o){
    if (!luabridge::push<VideoPlayer>(vm, o))
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
void LuaFunctionBase::push_value(lua_State *vm, ControllerColliderHit o){
    if (!luabridge::push<ControllerColliderHit>(vm, o))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);
}

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
