// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "object/GameObject.h"
#include "LuaBinding.h"
#include <optional>
#include "log/Logger.h"

#include <algorithm>
#include <set>

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Engine.h"
#include "device/Device.h"

#include <climits>
#include <cmath>
#include "Log.h"
#include "util/SubscriptionOwnerIndex.h"
#include "debug/DebugOverlay.h"
#include "debug/DebugStats.h"
#include "Scene.h"
#include "SceneManager.h"
#include "PrefabManager.h"
#include "PrefabAsset.h"
#include "asset/AssetMap.h"
#include "Input.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"
#include "input/InputManagerFormat.h"
#include "json.hpp"

#include <cstring>
#include <set>
#include <stdexcept>
#include "System.h"
#include "render/SystemRender.h"
#include "player/RunControl.h"
#include "Screen.h"
#include "util/CanvasScalerRef.h"
#include "registry/PhysicsLayers.h"
#include "math/Ray.h"
#include "picking/MeshRaycast.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "Contact2D.h"
#include "Manifold2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "Contact3D.h"
#include "CollideShapeResult3D.h"
#endif

#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "pool/ShaderPool.h"
#include "render/ObjectRender.h"
#include "shader/ShaderVariantCollection.h"
#include "shader/ShaderWarmup.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderGlobals.h"
#include "util/Color.h"
#include "object/EntityHandle.h"

using namespace jasperengine;

#define PBR_LUA_PROPERTY(name, lobe, Lobe, field) \
    .addProperty(name, &PbrLuaField<&MaterialPbrExtensions::lobe, &MaterialPbrExtensions::Lobe::field>::get, \
                 &PbrLuaField<&MaterialPbrExtensions::lobe, &MaterialPbrExtensions::Lobe::field>::set)
#define PBR_LUA_TEXTURE(name, lobe, Lobe, field) \
    .addProperty(name, &PbrLuaField<&MaterialPbrExtensions::lobe, &MaterialPbrExtensions::Lobe::field>::get)

namespace {
    // A field of Material::pbr as a flat Lua property of Material (MaterialPbr's table names
    // them): read without allocating, written into the material's own value (into range)
    template<auto Lobe, auto Field>
    struct PbrLuaField {
        using Value = std::decay_t<decltype(std::declval<MaterialPbrExtensions&>().*Lobe.*Field)>;
        static Value get(const Material* self) {
            return self->pbr.get().*Lobe.*Field;
        }
        static void set(Material* self, Value value) {
            MaterialPbrExtensions& pbr = self->pbr.edit();
            pbr.*Lobe.*Field = value;
            MaterialPbr::clamp(pbr);
        }
    };

    // Material.shadingModel: the Hair lobe's flag as the Lit's Shading Model
    ShadingModel materialShadingModel(const Material* self){
        return MaterialPbr::getShadingModel(self->pbr.get());
    }
    void setMaterialShadingModel(Material* self, ShadingModel model){
        MaterialPbr::setShadingModel(self->pbr.edit(), model);
    }

    // What Log.category(name) returns: a category's trace/debug/info/warn/error
    struct LuaLogCategory {
        LogCategory* category;
    };

    // a Lua traceback per Project Settings > Debug > Log Stack Traces (from the caller of the
    // logging function), "" when that level gets none
    std::string luaLogTraceback(lua_State* L, LogLevel level) {
        if (!Log::wantsStackTrace(level)) return std::string();
        luaL_traceback(L, L, nullptr, 1);
        size_t length = 0;
        const char* trace = lua_tolstring(L, -1, &length);
        std::string text(trace ? trace : "", trace ? length : 0);
        lua_pop(L, 1);
        return text;
    }

    void luaLog(lua_State* L, LogLevel level, const LogCategory& category, const char* text) {
        if (!Logger::shouldLog(level, category)) return;
        LuaBinding::logFromLua(L, level, category, text ? std::string_view(text) : std::string_view("(nil)"),
                               luaLogTraceback(L, level));
    }
}

// LayerMask.getMask("Default", "Enemies") or LayerMask.getMask({"Default", "Enemies"}).
// Unity returns an int: layer 31 makes it negative, as there.
static int luaLayerMaskGetMask(lua_State* L){
    std::vector<std::string> names;
    const int top = lua_gettop(L);
    if (top == 1 && lua_istable(L, 1)){
        const lua_Integer count = (lua_Integer)lua_rawlen(L, 1);
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) != LUA_TSTRING){
                return luaL_error(L, "LayerMask.getMask: layer names must be strings");
            }
            names.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }else{
        for (int i = 1; i <= top; i++){
            names.emplace_back(luaL_checkstring(L, i));
        }
    }
    lua_pushinteger(L, (lua_Integer)(int32_t)TagManager::getMask(names));
    return 1;
}

// Scene:findEntitiesWith / findChangedEntities / markChanged: component types by the name
// they were registered with ("Transform", "MeshComponent"). False (logged) on an unknown name
// or a value that is not a string.
static bool luaComponentId(lua_State* L, int index, Scene* scene, const char* function, ComponentId& id){
    if (lua_type(L, index) != LUA_TSTRING){
        Log::error("Scene:%s: component names must be strings", function);
        return false;
    }
    const std::string name = lua_tostring(L, index);
    if (!scene->findComponentId(name, id)){
        Log::error("Scene:%s: no component type '%s'", function, name.c_str());
        return false;
    }
    return true;
}

// a Lua list of component names at `index` (nothing there: an empty list)
static bool luaComponentIds(lua_State* L, int index, Scene* scene, const char* function, std::vector<ComponentId>& ids){
    if (lua_isnoneornil(L, index)){
        return true;
    }
    if (!lua_istable(L, index)){
        Log::error("Scene:%s: expected a table of component names", function);
        return false;
    }
    const lua_Integer count = (lua_Integer)lua_rawlen(L, index);
    for (lua_Integer i = 1; i <= count; i++){
        lua_rawgeti(L, index, i);
        ComponentId id;
        const bool ok = luaComponentId(L, -1, scene, function, id);
        lua_pop(L, 1);
        if (!ok){
            return false;
        }
        ids.push_back(id);
    }
    return true;
}

// Screen.setExtraInsets(left, top, right, bottom [, unit]): unit "canvas" (default) or "pixels"
static int luaScreenSetExtraInsets(lua_State* L){
    const char* edges[] = {"left", "top", "right", "bottom"};
    float values[4];
    for (int i = 0; i < 4; i++){
        if (lua_type(L, i + 1) != LUA_TNUMBER){
            return luaL_error(L, "Screen.setExtraInsets: %s must be a number (left, top, right, bottom [, \"canvas\"|\"pixels\"])", edges[i]);
        }
        values[i] = (float)lua_tonumber(L, i + 1);
    }
    bool pixels = false;
    if (!lua_isnoneornil(L, 5)){
        const char* unit = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : nullptr;
        if (unit && std::string(unit) == "pixels"){
            pixels = true;
        }else if (!unit || std::string(unit) != "canvas"){
            return luaL_error(L, "Screen.setExtraInsets: unit must be \"canvas\" or \"pixels\"");
        }
    }
    Screen::setExtraInsets(EdgeInsets(values[0], values[1], values[2], values[3]), pixels);
    return 0;
}

// ---- Input (Unity's Input class: edges, touches, the Input Manager's actions)
namespace{

    // a key, mouse button or gamepad button by its control name ("space", "mouse_left",
    // "gamepad_a", Unity's "left ctrl", "joystick button 0"...); one error per unknown name
    bool luaInputControl(const std::string& name, InputControl& control){
        static std::set<std::string> reported;
        std::string error;
        if (InputControlNames::parseButton(name, control, &error))
            return true;
        if (reported.insert(name).second)
            Log::error("Input: %s", error.c_str());
        return false;
    }

    bool luaControlHeld(const std::string& name){
        InputControl control;
        if (!luaInputControl(name, control)) return false;
        switch (control.device){
            case InputDevice::Key: return Input::isKeyPressed(control.code);
            case InputDevice::Mouse: return Input::isMousePressed(control.code);
            case InputDevice::GamepadButton:
                for (size_t i = 0; i < Input::numGamepads(); i++){
                    if (Input::isGamepadButtonPressed(Input::getGamepadId(i), control.code)) return true;
                }
                return false;
            default: return false;
        }
    }

    bool luaControlEdge(const std::string& name, bool down){
        InputControl control;
        if (!luaInputControl(name, control)) return false;
        switch (control.device){
            case InputDevice::Key: return down ? Input::getKeyDown(control.code) : Input::getKeyUp(control.code);
            case InputDevice::Mouse: return down ? Input::getMouseButtonDown(control.code) : Input::getMouseButtonUp(control.code);
            case InputDevice::GamepadButton: return down ? Input::getGamepadButtonDown(-1, control.code) : Input::getGamepadButtonUp(-1, control.code);
            default: return false;
        }
    }

    int luaGamepadButton(const std::string& name){
        const int button = InputControlNames::parseGamepadButton(name);
        if (button < 0){
            static std::set<std::string> reported;
            if (reported.insert(name).second)
                Log::error("Input: unknown gamepad button \"%s\" (gamepad_a, gamepad_dpad_up, ... or Input.GAMEPAD_BUTTON_*)", name.c_str());
        }
        return button;
    }

    // the owner of a simulated source: a name (default "script") or a script instance (`self`),
    // whose sources go when the instance is released
    uintptr_t luaSourceOwner(const luabridge::LuaRef& source){
        if (source.isTable()){
            lua_State* L = source.state();
            source.push(L);
            const void* table = lua_topointer(L, -1);
            lua_pop(L, 1);
            return reinterpret_cast<uintptr_t>(table);
        }
        if (source.isString())
            return InputActions::namedSource(source.unsafe_cast<std::string>());
        return InputActions::namedSource("script");
    }

    // JSON <-> Lua tables, for the bindings (getBindings / setBindings)
    void luaPushJson(lua_State* L, const nlohmann::json& value){
        if (value.is_object()){
            lua_newtable(L);
            for (auto it = value.begin(); it != value.end(); ++it){
                luaPushJson(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
        }else if (value.is_array()){
            lua_newtable(L);
            for (size_t i = 0; i < value.size(); i++){
                luaPushJson(L, value[i]);
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            }
        }else if (value.is_boolean()){
            lua_pushboolean(L, value.get<bool>() ? 1 : 0);
        }else if (value.is_number_integer()){
            lua_pushinteger(L, (lua_Integer)value.get<long long>());
        }else if (value.is_number()){
            lua_pushnumber(L, value.get<double>());
        }else if (value.is_string()){
            const std::string& text = value.get_ref<const std::string&>();
            lua_pushlstring(L, text.data(), text.size());
        }else{
            lua_pushnil(L);
        }
    }

    // a table with keys 1..n is a list (an empty table too), else an object
    nlohmann::json luaToJson(lua_State* L, int index, int depth = 0){
        index = lua_absindex(L, index);
        switch (lua_type(L, index)){
            case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)) return (long long)lua_tointeger(L, index);
                return lua_tonumber(L, index);
            case LUA_TSTRING: {
                size_t length = 0;
                const char* text = lua_tolstring(L, index, &length);
                return std::string(text, length);
            }
            case LUA_TTABLE: {
                if (depth > 16) return nullptr;
                const lua_Integer length = (lua_Integer)lua_rawlen(L, index);
                size_t keys = 0;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    keys++;
                    lua_pop(L, 1);
                }
                if ((lua_Integer)keys == length){
                    nlohmann::json list = nlohmann::json::array();
                    for (lua_Integer i = 1; i <= length; i++){
                        lua_rawgeti(L, index, i);
                        list.push_back(luaToJson(L, -1, depth + 1));
                        lua_pop(L, 1);
                    }
                    return list;
                }
                nlohmann::json object = nlohmann::json::object();
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    if (lua_type(L, -2) == LUA_TSTRING)
                        object[lua_tostring(L, -2)] = luaToJson(L, -1, depth + 1);
                    lua_pop(L, 1);
                }
                return object;
            }
            default: return nullptr;
        }
    }

    // Input.getBindings(name): the bindings in use, as tables of the Input Manager's shape
    int luaInputGetBindings(lua_State* L){
        const std::string name = luaL_checkstring(L, 1);
        if (!InputActions::hasAction(name)){
            InputActions::getBindings(name);   // logs the unknown name once
            lua_newtable(L);
            return 1;
        }
        luaPushJson(L, nlohmann::json::parse(InputManagerFormat::bindingsToJson(InputActions::getBindings(name))));
        return 1;
    }

    // Input.setBindings(name, bindings) -> true | false, error
    int luaInputSetBindings(lua_State* L){
        const std::string name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        std::vector<InputManagerBinding> bindings;
        std::string error;
        if (!InputActions::hasAction(name)){
            error = "Input action \"" + name + "\" does not exist (Project Settings > Input)";
        }else if (InputManagerFormat::parseBindingsJson(luaToJson(L, 2).dump(), bindings, &error, name) &&
                   InputActions::setBindings(name, bindings, &error)){
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    // Input.importBindings(json) -> true | false, error
    int luaInputImportBindings(lua_State* L){
        const std::string json = luaL_checkstring(L, 1);
        std::string error;
        if (InputActions::importOverrides(json, &error)){
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushboolean(L, 0);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    // Input.anyKeyDown is a property (Unity: `if Input.anyKeyDown then`) and a function (S22:
    // `Input.anyKeyDown()`): calling a boolean with no arguments gives it back. With arguments it
    // is the usual error
    int luaCallBoolean(lua_State* L){
        if (lua_gettop(L) != 1)
            return luaL_error(L, "attempt to call a boolean value");
        lua_pushvalue(L, 1);
        return 1;
    }

    void luaMakeBooleansCallable(lua_State* L){
        lua_pushboolean(L, 0);
        if (!lua_getmetatable(L, -1))
            lua_newtable(L);
        lua_pushcfunction(L, &luaCallBoolean);
        lua_setfield(L, -2, "__call");
        lua_setmetatable(L, -2);
        lua_pop(L, 1);
    }

}

// Shader: global shader values (Unity's Shader.SetGlobal* / PropertyToID). A global is named
// by a string or by the integer Shader.propertyToID gave; setters return false (and log once)
// when the global is of another kind (a matrix, an array, a texture)
namespace{

    // a binding's global argument: a name, or nil to unbind (anything else is an error)
    std::string luaBindingGlobal(const luabridge::LuaRef& global, const char* function){
        if (global.isNil())
            return std::string();
        if (global.isString())
            return global.unsafe_cast<std::string>();
        throw std::runtime_error(std::string(function) + ": the global is a name (a string), or nil to unbind");
    }

    // the global argument 1 names: a string (made on first use by a setter), or an id
    int luaGlobalArg(lua_State* L, const char* function, bool create){
        if (lua_type(L, 1) == LUA_TNUMBER){
            if (!lua_isinteger(L, 1))
                luaL_error(L, "Shader.%s: an id must be an integer (Shader.propertyToID)", function);
            const int id = (int)lua_tointeger(L, 1);
            if (ShaderGlobals::nameOf(id).empty())
                luaL_error(L, "Shader.%s: %d is not an id Shader.propertyToID gave", function, id);
            return id;
        }
        if (lua_type(L, 1) != LUA_TSTRING)
            luaL_error(L, "Shader.%s: argument 1 must be a global's name or id", function);
        const std::string name = lua_tostring(L, 1);
        std::string error;
        if (create && !ShaderGlobals::isValidName(name, &error))
            luaL_error(L, "Shader.%s: %s", function, error.c_str());
        return create ? ShaderGlobals::propertyToID(name) : ShaderGlobals::findID(name);
    }

    // a value of a registered class, nil when it cannot be pushed
    template<typename T>
    void luaPushValue(lua_State* L, const T& value){
        if (!luabridge::push(L, value))
            lua_pushnil(L);
    }

    // a Vector4, Vector3 (w = wFill) or Vector2 at index
    bool luaVectorArg(lua_State* L, int index, Vector4& out, float wFill, bool allowVector2){
        if (luabridge::isInstance<Vector4>(L, index)){
            auto value = luabridge::get<Vector4>(L, index);
            if (value){ out = *value; return true; }
        }else if (luabridge::isInstance<Vector3>(L, index)){
            auto value = luabridge::get<Vector3>(L, index);
            if (value){ const Vector3 v = *value; out = Vector4(v.x, v.y, v.z, wFill); return true; }
        }else if (allowVector2 && luabridge::isInstance<Vector2>(L, index)){
            auto value = luabridge::get<Vector2>(L, index);
            if (value){ const Vector2 v = *value; out = Vector4(v.x, v.y, 0.0f, 0.0f); return true; }
        }
        return false;
    }

    int luaShaderPropertyToID(lua_State* L){
        const std::string name = luaL_checkstring(L, 1);
        std::string error;
        if (!ShaderGlobals::isValidName(name, &error))
            return luaL_error(L, "Shader.propertyToID: %s", error.c_str());
        lua_pushinteger(L, ShaderGlobals::propertyToID(name));
        return 1;
    }

    int luaShaderSetGlobalFloat(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalFloat", true);
        if (!lua_isnumber(L, 2))
            return luaL_error(L, "Shader.setGlobalFloat: argument 2 must be a number");
        lua_pushboolean(L, ShaderGlobals::setFloat(id, (float)lua_tonumber(L, 2)) ? 1 : 0);
        return 1;
    }

    // Unity's SetGlobalInt stores a float
    int luaShaderSetGlobalInt(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalInt", true);
        if (!lua_isnumber(L, 2))
            return luaL_error(L, "Shader.setGlobalInt: argument 2 must be an integer");
        lua_pushboolean(L, ShaderGlobals::setFloat(id, (float)(lua_Integer)lua_tonumber(L, 2)) ? 1 : 0);
        return 1;
    }

    int luaShaderSetGlobalVector(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalVector", true);
        Vector4 value;
        if (!luaVectorArg(L, 2, value, 0.0f, true))
            return luaL_error(L, "Shader.setGlobalVector: argument 2 must be a Vector4, Vector3 or Vector2");
        lua_pushboolean(L, ShaderGlobals::setVector(id, value) ? 1 : 0);
        return 1;
    }

    int luaShaderSetGlobalColor(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalColor", true);
        Vector4 value;
        if (!luaVectorArg(L, 2, value, 1.0f, false))
            return luaL_error(L, "Shader.setGlobalColor: argument 2 must be a Vector4 or Vector3 (sRGB)");
        lua_pushboolean(L, ShaderGlobals::setColor(id, value) ? 1 : 0);
        return 1;
    }

    int luaShaderSetGlobalMatrix(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalMatrix", true);
        if (!luabridge::isInstance<Matrix4>(L, 2))
            return luaL_error(L, "Shader.setGlobalMatrix: argument 2 must be a Matrix4");
        auto value = luabridge::get<Matrix4>(L, 2);
        lua_pushboolean(L, value && ShaderGlobals::setMatrix(id, *value) ? 1 : 0);
        return 1;
    }

    int luaShaderSetGlobalFloatArray(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalFloatArray", true);
        luaL_checktype(L, 2, LUA_TTABLE);
        const lua_Integer count = (lua_Integer)lua_rawlen(L, 2);
        if (count < 1)
            return luaL_error(L, "Shader.setGlobalFloatArray: the table needs 1 to %d numbers", (int)ShaderGlobals::MAX_ARRAY_SIZE);
        std::vector<float> values;
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, 2, i);
            if (!lua_isnumber(L, -1))
                return luaL_error(L, "Shader.setGlobalFloatArray: element %d must be a number", (int)i);
            values.push_back((float)lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pushboolean(L, ShaderGlobals::setFloatArray(id, values.data(), values.size()) ? 1 : 0);
        return 1;
    }

    int luaShaderSetGlobalVectorArray(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalVectorArray", true);
        luaL_checktype(L, 2, LUA_TTABLE);
        const lua_Integer count = (lua_Integer)lua_rawlen(L, 2);
        if (count < 1)
            return luaL_error(L, "Shader.setGlobalVectorArray: the table needs 1 to %d vectors", (int)ShaderGlobals::MAX_ARRAY_SIZE);
        std::vector<Vector4> values;
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, 2, i);
            Vector4 value;
            if (!luaVectorArg(L, lua_gettop(L), value, 0.0f, true))
                return luaL_error(L, "Shader.setGlobalVectorArray: element %d must be a Vector4, Vector3 or Vector2", (int)i);
            values.push_back(value);
            lua_pop(L, 1);
        }
        lua_pushboolean(L, ShaderGlobals::setVectorArray(id, values.data(), values.size()) ? 1 : 0);
        return 1;
    }

    // a Texture, an image path, a Framebuffer (a camera's render texture); nil: the default again
    int luaShaderSetGlobalTexture(lua_State* L){
        const int id = luaGlobalArg(L, "setGlobalTexture", true);
        if (lua_isnoneornil(L, 2)){
            lua_pushboolean(L, ShaderGlobals::clearTexture(id) ? 1 : 0);
            return 1;
        }
        Texture texture;
        if (lua_type(L, 2) == LUA_TSTRING){
            texture.setPath(lua_tostring(L, 2));
        }else if (luabridge::isInstance<Texture>(L, 2)){
            auto value = luabridge::get<Texture>(L, 2);
            if (value) texture = *value;
        }else if (luabridge::isInstance<Framebuffer>(L, 2)){
            auto value = luabridge::get<Framebuffer*>(L, 2);
            if (value && *value) texture.setFramebuffer(*value);
        }else{
            return luaL_error(L, "Shader.setGlobalTexture: argument 2 must be a Texture, an image path, a Framebuffer or nil");
        }
        lua_pushboolean(L, ShaderGlobals::setTexture(id, texture) ? 1 : 0);
        return 1;
    }

    // the value of the global argument 1 names: false when it does not exist or is a texture
    bool luaGlobalValue(lua_State* L, const char* function, ShaderGlobalValue& value){
        const int id = luaGlobalArg(L, function, false);
        return ShaderGlobals::get(id, value) && !ShaderGlobals::isTexture(value.type) && !value.values.empty();
    }

    // its first slot (a float in .x); false for a matrix too
    bool luaGlobalLeading(lua_State* L, const char* function, Vector4& out){
        ShaderGlobalValue value;
        if (!luaGlobalValue(L, function, value) || value.type == ShaderGlobalType::Matrix4)
            return false;
        out = value.values[0];
        return true;
    }

    int luaShaderGetGlobalFloat(lua_State* L){
        Vector4 value;
        if (luaGlobalLeading(L, "getGlobalFloat", value)) lua_pushnumber(L, value.x);
        else lua_pushnil(L);
        return 1;
    }

    int luaShaderGetGlobalInt(lua_State* L){
        Vector4 value;
        if (luaGlobalLeading(L, "getGlobalInt", value)) lua_pushinteger(L, (lua_Integer)value.x);
        else lua_pushnil(L);
        return 1;
    }

    int luaShaderGetGlobalVector(lua_State* L){
        Vector4 value;
        if (luaGlobalLeading(L, "getGlobalVector", value)) luaPushValue(L, value);
        else lua_pushnil(L);
        return 1;
    }

    // as sRGB (alpha as is)
    int luaShaderGetGlobalColor(lua_State* L){
        Vector4 value;
        if (luaGlobalLeading(L, "getGlobalColor", value)) luaPushValue(L, Color::linearTosRGB(value));
        else lua_pushnil(L);
        return 1;
    }

    int luaShaderGetGlobalMatrix(lua_State* L){
        ShaderGlobalValue value;
        if (!luaGlobalValue(L, "getGlobalMatrix", value) || value.type != ShaderGlobalType::Matrix4 || value.values.size() < 4){
            lua_pushnil(L);
            return 1;
        }
        Matrix4 matrix;
        for (unsigned int c = 0; c < 4; c++)
            matrix.setColumn(c, value.values[c]);
        luaPushValue(L, matrix);
        return 1;
    }

    int luaShaderGetGlobalArray(lua_State* L, const char* function, bool vectors){
        ShaderGlobalValue value;
        if (!luaGlobalValue(L, function, value) || !ShaderGlobals::isArray(value.type)){
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, (int)value.values.size(), 0);
        for (size_t i = 0; i < value.values.size(); i++){
            if (vectors)
                luaPushValue(L, value.values[i]);
            else
                lua_pushnumber(L, value.values[i].x);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        return 1;
    }
    int luaShaderGetGlobalFloatArray(lua_State* L){ return luaShaderGetGlobalArray(L, "getGlobalFloatArray", false); }
    int luaShaderGetGlobalVectorArray(lua_State* L){ return luaShaderGetGlobalArray(L, "getGlobalVectorArray", true); }

    // the Texture a texture global holds (nil for none, its default unset, or another kind)
    int luaShaderGetGlobalTexture(lua_State* L){
        ShaderGlobalValue value;
        const int id = luaGlobalArg(L, "getGlobalTexture", false);
        if (!ShaderGlobals::get(id, value) || !ShaderGlobals::isTexture(value.type) || value.texture.empty()){
            lua_pushnil(L);
            return 1;
        }
        luaPushValue(L, value.texture);
        return 1;
    }

    int luaShaderHasGlobal(lua_State* L){
        const int id = luaGlobalArg(L, "hasGlobal", false);
        lua_pushboolean(L, ShaderGlobals::exists(id) ? 1 : 0);
        return 1;
    }

    int luaShaderGetGlobalNames(lua_State* L){
        const std::vector<std::string> names = ShaderGlobals::getNames();
        lua_createtable(L, (int)names.size(), 0);
        for (size_t i = 0; i < names.size(); i++){
            lua_pushstring(L, names[i].c_str());
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        return 1;
    }

}

// Physics and Physics2D. Layer masks are read as Unity's int and any integer is taken by
// its low 32 bits, so LayerMask.getMask(...), ~mask and -1 (Everything) all work.
namespace{

    uint32_t luaOptLayerMask(lua_State* L, int index, uint32_t defaultMask){
        if (lua_isnoneornil(L, index)){
            return defaultMask;
        }
        return (uint32_t)luaL_checkinteger(L, index);
    }

    lua_Integer luaMaskToInteger(uint32_t mask){
        return (lua_Integer)(int32_t)mask;
    }

    lua_Integer luaAllLayers(){ return luaMaskToInteger(Physics::ALL_LAYERS); }
    lua_Integer luaDefaultRaycastLayers(){ return luaMaskToInteger(Physics::DEFAULT_RAYCAST_LAYERS); }
    lua_Integer luaIgnoreRaycastLayer(){ return luaMaskToInteger(Physics::IGNORE_RAYCAST_LAYER); }

    template<typename PhysicsT>
    int luaIgnoreLayerCollision(lua_State* L){
        const int layer1 = (int)luaL_checkinteger(L, 1);
        const int layer2 = (int)luaL_checkinteger(L, 2);
        const bool ignore = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
        PhysicsT::ignoreLayerCollision(layer1, layer2, ignore);
        return 0;
    }

    template<typename PhysicsT>
    int luaGetIgnoreLayerCollision(lua_State* L){
        const int layer1 = (int)luaL_checkinteger(L, 1);
        const int layer2 = (int)luaL_checkinteger(L, 2);
        lua_pushboolean(L, PhysicsT::getIgnoreLayerCollision(layer1, layer2) ? 1 : 0);
        return 1;
    }

    template<typename PhysicsT>
    int luaGetLayerCollisionMask(lua_State* L){
        const int layer = (int)luaL_checkinteger(L, 1);
        lua_pushinteger(L, luaMaskToInteger(PhysicsT::getLayerCollisionMask(layer)));
        return 1;
    }

    template<typename PhysicsT>
    int luaSetLayerCollisionMask(lua_State* L){
        const int layer = (int)luaL_checkinteger(L, 1);
        const uint32_t mask = (uint32_t)luaL_checkinteger(L, 2);
        PhysicsT::setLayerCollisionMask(layer, mask);
        return 0;
    }

    template<typename PhysicsT>
    int luaResetLayerCollisionMatrix(lua_State* L){
        (void)L;
        PhysicsT::resetLayerCollisionMatrix();
        return 0;
    }

    // Physics.ignoreCollision(a, b [, ignore = true]) / getIgnoreCollision(a, b): a and b are
    // GameObjects or bodies (any EntityHandle) with a Rigidbody (2D: Rigidbody 2D) of one scene
    template<bool is2D>
    bool luaBodyPairArgs(lua_State* L, const char* function, Scene*& scene, Entity& a, Entity& b){
        const char* kind = is2D ? "Rigidbody 2D" : "Rigidbody";
        Entity entities[2] = {NULL_ENTITY, NULL_ENTITY};
        Scene* scenes[2] = {nullptr, nullptr};
        for (int i = 0; i < 2; i++){
            EntityHandle* handle = nullptr;
            if (luabridge::isInstance<EntityHandle>(L, i + 1)){
                auto result = luabridge::get<EntityHandle*>(L, i + 1);
                if (result) handle = result.value();
            }
            if (!handle || !handle->getScene() || !handle->getScene()->isEntityCreated(handle->getEntity())){
                luaL_error(L, "%s: argument %d must be a GameObject or %s", function, i + 1, kind);
                return false;
            }
            scenes[i] = handle->getScene();
            entities[i] = handle->getEntity();
            bool hasBody = false;
            if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
                hasBody = scenes[i]->findComponent<Rigidbody2DComponent>(entities[i]) != nullptr;
#endif
            }else{
#ifdef JASPERENGINE_PHYSICS_3D
                hasBody = scenes[i]->findComponent<RigidbodyComponent>(entities[i]) != nullptr;
#endif
            }
            if (!hasBody){
                luaL_error(L, "%s: argument %d ('%s') has no %s", function, i + 1, scenes[i]->getEntityName(entities[i]).c_str(), kind);
                return false;
            }
        }
        if (scenes[0] != scenes[1]){
            luaL_error(L, "%s: the two bodies are in different scenes", function);
            return false;
        }
        if (entities[0] == entities[1]){
            luaL_error(L, "%s: a body with itself", function);
            return false;
        }
        scene = scenes[0];
        a = entities[0];
        b = entities[1];
        return true;
    }

    template<bool is2D>
    int luaIgnoreCollision(lua_State* L){
        const char* function = is2D ? "Physics2D.ignoreCollision" : "Physics.ignoreCollision";
        Scene* scene = nullptr;
        Entity a = NULL_ENTITY, b = NULL_ENTITY;
        if (!luaBodyPairArgs<is2D>(L, function, scene, a, b)){
            return 0;
        }
        [[maybe_unused]] const bool ignore = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
        if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
            scene->getSystem<PhysicsSystem>()->setIgnoreCollision2D(a, b, ignore);
#endif
        }else{
#ifdef JASPERENGINE_PHYSICS_3D
            scene->getSystem<PhysicsSystem>()->setIgnoreCollision3D(a, b, ignore);
#endif
        }
        return 0;
    }

    template<bool is2D>
    int luaGetIgnoreCollision(lua_State* L){
        const char* function = is2D ? "Physics2D.getIgnoreCollision" : "Physics.getIgnoreCollision";
        Scene* scene = nullptr;
        Entity a = NULL_ENTITY, b = NULL_ENTITY;
        if (!luaBodyPairArgs<is2D>(L, function, scene, a, b)){
            return 0;
        }
        bool ignored = false;
        if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
            ignored = scene->getSystem<PhysicsSystem>()->getIgnoreCollision2D(a, b);
#endif
        }else{
#ifdef JASPERENGINE_PHYSICS_3D
            ignored = scene->getSystem<PhysicsSystem>()->getIgnoreCollision3D(a, b);
#endif
        }
        lua_pushboolean(L, ignored ? 1 : 0);
        return 1;
    }

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
    Scene* luaSceneArg(lua_State* L, const char* function){
        Scene* scene = nullptr;
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result){
                scene = result.value();
            }
        }
        if (!scene){
            luaL_error(L, "%s: argument 1 must be a Scene", function);
        }
        return scene;
    }

    // get<T> raises a Lua error on a mismatch, so the type is tested first
    bool luaRayArg(lua_State* L, int index, Ray& ray){
        if (!luabridge::isInstance<Ray>(L, index)){
            return false;
        }
        auto result = luabridge::get<Ray>(L, index);
        if (!result){
            return false;
        }
        ray = result.value();
        return true;
    }

    void luaPushRayReturn(lua_State* L, const RayReturn& hit){
        if (!luabridge::push(L, hit)){
            lua_pushnil(L);
        }
    }

    void luaPushRayReturns(lua_State* L, const std::vector<RayReturn>& hits){
        lua_createtable(L, (int)hits.size(), 0);
        for (size_t i = 0; i < hits.size(); i++){
            luaPushRayReturn(L, hits[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
    }

    // Physics.raycast(scene, ray [, layerMask]) or (scene, origin, direction, maxDistance [, layerMask]),
    // and the 2D versions with Vector2 origin and direction
    template<typename VectorT, bool all, bool is2D>
    int luaRaycast(lua_State* L){
        const char* function = is2D ? (all ? "Physics2D.raycastAll" : "Physics2D.raycast")
                                    : (all ? "Physics.raycastAll" : "Physics.raycast");
        Scene* scene = luaSceneArg(L, function);

        Ray ray;
        uint32_t layerMask;
        if (luaRayArg(L, 2, ray)){
            layerMask = luaOptLayerMask(L, 3, Physics::DEFAULT_RAYCAST_LAYERS);
        }else{
            if (!luabridge::isInstance<VectorT>(L, 2) || !luabridge::isInstance<VectorT>(L, 3)){
                return luaL_error(L, "%s: expected (scene, ray [, layerMask]) or (scene, origin, direction, distance [, layerMask])", function);
            }
            auto origin = luabridge::get<VectorT>(L, 2);
            auto direction = luabridge::get<VectorT>(L, 3);
            if (!origin || !direction){
                return luaL_error(L, "%s: expected (scene, ray [, layerMask]) or (scene, origin, direction, distance [, layerMask])", function);
            }
            const float maxDistance = (float)luaL_checknumber(L, 4);
            layerMask = luaOptLayerMask(L, 5, Physics::DEFAULT_RAYCAST_LAYERS);

            const VectorT originValue = origin.value();
            const VectorT directionValue = direction.value();
            if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
                if (all){
                    luaPushRayReturns(L, Physics2D::raycastAll(scene, originValue, directionValue, maxDistance, layerMask));
                }else{
                    luaPushRayReturn(L, Physics2D::raycast(scene, originValue, directionValue, maxDistance, layerMask));
                }
#endif
            }else{
#ifdef JASPERENGINE_PHYSICS_3D
                if (all){
                    luaPushRayReturns(L, Physics::raycastAll(scene, originValue, directionValue, maxDistance, layerMask));
                }else{
                    luaPushRayReturn(L, Physics::raycast(scene, originValue, directionValue, maxDistance, layerMask));
                }
#endif
            }
            return 1;
        }

        if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
            if (all){
                luaPushRayReturns(L, Physics2D::raycastAll(scene, ray, layerMask));
            }else{
                luaPushRayReturn(L, Physics2D::raycast(scene, ray, layerMask));
            }
#endif
        }else{
#ifdef JASPERENGINE_PHYSICS_3D
            if (all){
                luaPushRayReturns(L, Physics::raycastAll(scene, ray, layerMask));
            }else{
                luaPushRayReturn(L, Physics::raycast(scene, ray, layerMask));
            }
#endif
        }
        return 1;
    }
#endif


    // ---- MeshRaycast (docs/scripting/mesh-raycast.md): Physics.raycast's arguments and
    // distance semantics, against what is drawn

    Scene* luaMeshRaycastScene(lua_State* L, const char* function){
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result && result.value()){
                return result.value();
            }
        }
        luaL_error(L, "%s: argument 1 must be a Scene", function);
        return nullptr;
    }

    bool luaMeshRaycastRay(lua_State* L, int index, Ray& ray){
        if (!luabridge::isInstance<Ray>(L, index)){
            return false;
        }
        auto result = luabridge::get<Ray>(L, index);
        if (!result){
            return false;
        }
        ray = result.value();
        return true;
    }

    // an entity number or a GameObject (any EntityHandle)
    bool luaMeshRaycastEntity(lua_State* L, int index, Entity& entity){
        if (lua_isinteger(L, index)){
            entity = (Entity)lua_tointeger(L, index);
            return true;
        }
        if (luabridge::isInstance<EntityHandle>(L, index)){
            auto handle = luabridge::get<EntityHandle*>(L, index);
            if (handle && handle.value()){
                entity = handle.value()->getEntity();
                return true;
            }
        }
        return false;
    }

    // { hitBackfaces = false, includeInactive = false, ignore = {e1, e2}, kinds = MeshRaycast.MESH | ... }
    MeshRaycastOptions luaMeshRaycastOptions(lua_State* L, int index, const char* function){
        MeshRaycastOptions options;
        if (lua_isnoneornil(L, index)){
            return options;
        }
        if (!lua_istable(L, index)){
            luaL_error(L, "%s: options must be a table", function);
            return options;
        }
        index = lua_absindex(L, index);
        lua_getfield(L, index, "hitBackfaces");
        if (!lua_isnil(L, -1)) options.hitBackfaces = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, index, "includeInactive");
        if (!lua_isnil(L, -1)) options.includeInactive = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, index, "kinds");
        if (!lua_isnil(L, -1)) options.kinds = (uint32_t)luaL_checkinteger(L, -1) & MeshRaycast::ALL;
        lua_pop(L, 1);
        lua_getfield(L, index, "ignore");
        if (lua_istable(L, -1)){
            const lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= count; i++){
                lua_rawgeti(L, -1, i);
                Entity entity = NULL_ENTITY;
                if (luaMeshRaycastEntity(L, -1, entity)){
                    options.ignore.push_back(entity);
                }else{
                    lua_pop(L, 2);
                    luaL_error(L, "%s: options.ignore[%d] is not an entity", function, (int)i);
                    return options;
                }
                lua_pop(L, 1);
            }
        }else if (!lua_isnil(L, -1)){
            lua_pop(L, 1);
            luaL_error(L, "%s: options.ignore must be a table of entities", function);
            return options;
        }
        lua_pop(L, 1);
        return options;
    }

    void luaPushMeshRaycastHit(lua_State* L, const MeshRaycastHit& hit){
        if (!luabridge::push(L, hit)){
            lua_pushnil(L);
        }
    }

    // raycast / raycastAll: (scene, ray [, layerMask [, options]]) or
    // (scene, origin, direction, maxDistance [, layerMask [, options]])
    template<bool all>
    int luaMeshRaycast(lua_State* L){
        const char* function = all ? "MeshRaycast.raycastAll" : "MeshRaycast.raycast";
        Scene* scene = luaMeshRaycastScene(L, function);
        Ray ray;
        if (luaMeshRaycastRay(L, 2, ray)){
            const uint32_t layerMask = luaOptLayerMask(L, 3, LAYER_MASK_DEFAULT_RAYCAST);
            const MeshRaycastOptions options = luaMeshRaycastOptions(L, 4, function);
            if (all){
                const std::vector<MeshRaycastHit> hits = MeshRaycast::raycastAll(scene, ray, layerMask, options);
                lua_createtable(L, (int)hits.size(), 0);
                for (size_t i = 0; i < hits.size(); i++){
                    luaPushMeshRaycastHit(L, hits[i]);
                    lua_rawseti(L, -2, (lua_Integer)(i + 1));
                }
            }else{
                luaPushMeshRaycastHit(L, MeshRaycast::raycast(scene, ray, layerMask, options));
            }
            return 1;
        }
        if (!luabridge::isInstance<Vector3>(L, 2) || !luabridge::isInstance<Vector3>(L, 3)){
            return luaL_error(L, "%s: expected (scene, ray [, layerMask [, options]]) or (scene, origin, direction, maxDistance [, layerMask [, options]])", function);
        }
        auto origin = luabridge::get<Vector3>(L, 2);
        auto direction = luabridge::get<Vector3>(L, 3);
        if (!origin || !direction){
            return luaL_error(L, "%s: expected (scene, ray [, layerMask [, options]]) or (scene, origin, direction, maxDistance [, layerMask [, options]])", function);
        }
        const float maxDistance = (float)luaL_checknumber(L, 4);
        const uint32_t layerMask = luaOptLayerMask(L, 5, LAYER_MASK_DEFAULT_RAYCAST);
        const MeshRaycastOptions options = luaMeshRaycastOptions(L, 6, function);
        if (all){
            const std::vector<MeshRaycastHit> hits = MeshRaycast::raycastAll(scene, origin.value(), direction.value(), maxDistance, layerMask, options);
            lua_createtable(L, (int)hits.size(), 0);
            for (size_t i = 0; i < hits.size(); i++){
                luaPushMeshRaycastHit(L, hits[i]);
                lua_rawseti(L, -2, (lua_Integer)(i + 1));
            }
        }else{
            luaPushMeshRaycastHit(L, MeshRaycast::raycast(scene, origin.value(), direction.value(), maxDistance, layerMask, options));
        }
        return 1;
    }

    // raycastEntity(scene, entity, ray [, options])
    int luaMeshRaycastEntityHit(lua_State* L){
        const char* function = "MeshRaycast.raycastEntity";
        Scene* scene = luaMeshRaycastScene(L, function);
        Entity entity = NULL_ENTITY;
        if (!luaMeshRaycastEntity(L, 2, entity)){
            return luaL_error(L, "%s: argument 2 must be an entity", function);
        }
        Ray ray;
        if (!luaMeshRaycastRay(L, 3, ray)){
            return luaL_error(L, "%s: argument 3 must be a Ray", function);
        }
        luaPushMeshRaycastHit(L, MeshRaycast::raycastEntity(scene, entity, ray, luaMeshRaycastOptions(L, 4, function)));
        return 1;
    }

    // prepare(scene [, entities])
    int luaMeshRaycastPrepare(lua_State* L){
        const char* function = "MeshRaycast.prepare";
        Scene* scene = luaMeshRaycastScene(L, function);
        std::vector<Entity> entities;
        if (lua_istable(L, 2)){
            const lua_Integer count = (lua_Integer)lua_rawlen(L, 2);
            for (lua_Integer i = 1; i <= count; i++){
                lua_rawgeti(L, 2, i);
                Entity entity = NULL_ENTITY;
                if (!luaMeshRaycastEntity(L, -1, entity)){
                    lua_pop(L, 1);
                    return luaL_error(L, "%s: entities[%d] is not an entity", function, (int)i);
                }
                entities.push_back(entity);
                lua_pop(L, 1);
            }
        }else if (!lua_isnoneornil(L, 2)){
            return luaL_error(L, "%s: argument 2 must be a table of entities", function);
        }
        MeshRaycast::prepare(scene, entities);
        return 0;
    }

    lua_Integer luaMeshRaycastHitKind(const MeshRaycastHit* hit){ return (lua_Integer)(1u << (uint32_t)hit->kind); }
    lua_Integer luaMeshRaycastHitEntity(const MeshRaycastHit* hit){ return (lua_Integer)hit->entity; }
    lua_Integer luaMeshRaycastHitSubmesh(const MeshRaycastHit* hit){ return (lua_Integer)hit->submesh; }
    lua_Integer luaMeshRaycastHitTriangle(const MeshRaycastHit* hit){ return (lua_Integer)hit->triangle; }
    lua_Integer luaMeshRaycastHitInstance(const MeshRaycastHit* hit){ return (lua_Integer)hit->instance; }
    lua_Integer luaMeshRaycastMesh(){ return MeshRaycast::MESH; }
    lua_Integer luaMeshRaycastInstance(){ return MeshRaycast::INSTANCE; }
    lua_Integer luaMeshRaycastTerrain(){ return MeshRaycast::TERRAIN; }
    lua_Integer luaMeshRaycastUI(){ return MeshRaycast::UI; }
    lua_Integer luaMeshRaycastBounds(){ return MeshRaycast::BOUNDS; }
    lua_Integer luaMeshRaycastAll(){ return MeshRaycast::ALL; }
}

// ---- Log and DebugOverlay (feature debug-overlay). Lua is built as C: lua_error longjmps over
// C++ destructors, so every check that can raise comes before the first C++ object.
namespace {

    struct LogLevelNames {};

    // Log.print/info/verbose/debug/warn/error(...): category lua, the values joined with tabs
    // (tostring), the calling chunk and line as the source (S09 R5) and a Lua traceback per
    // Project Settings > Debug > Log Stack Traces. Gated first: a filtered call builds nothing.
    int luaLogLine(lua_State* L, LogLevel level){
        LogCategory& category = logCategoryLua();
        if (!Logger::shouldLog(level, category))
            return 0;
        const int count = lua_gettop(L);
        luaL_Buffer buffer;
        luaL_buffinit(L, &buffer);
        if (count == 0)
            luaL_addstring(&buffer, "(nil)");
        for (int i = 1; i <= count; i++){
            if (i > 1)
                luaL_addchar(&buffer, '\t');
            luaL_tolstring(L, i, nullptr);
            luaL_addvalue(&buffer);
        }
        luaL_pushresult(&buffer);
        if (Log::wantsStackTrace(level))
            luaL_traceback(L, L, nullptr, 1);
        else
            lua_pushliteral(L, "");
        size_t messageLength = 0, traceLength = 0;
        const char* message = lua_tolstring(L, -2, &messageLength);
        const char* trace = lua_tolstring(L, -1, &traceLength);
        {
            const std::string messageText(message, messageLength);
            const std::string traceText(trace, traceLength);
            lua_pop(L, 2);
            LuaBinding::logFromLua(L, level, category, messageText, traceText);
        }
        return 0;
    }

    int luaLogPrint(lua_State* L){ return luaLogLine(L, LogLevel::Info); }
    int luaLogVerbose(lua_State* L){ return luaLogLine(L, LogLevel::Trace); }
    // compiled out of release builds, as Log::debug
    int luaLogDebug(lua_State* L){
    #ifndef NDEBUG
        return luaLogLine(L, LogLevel::Debug);
    #else
        (void)L;
        return 0;
    #endif
    }
    int luaLogWarn(lua_State* L){ return luaLogLine(L, LogLevel::Warn); }
    int luaLogError(lua_State* L){ return luaLogLine(L, LogLevel::Error); }

    // Log.getEntries([maxCount [, levels]]): the buffered lines, newest last
    int luaLogGetEntries(lua_State* L){
        lua_Integer maxCount = 0;
        if (!lua_isnoneornil(L, 1)){
            maxCount = luaL_checkinteger(L, 1);
            if (maxCount < 0)
                return luaL_argerror(L, 1, "must be 0 (all) or more");
        }
        uint32_t mask = LOG_LEVELS_ALL;
        if (!lua_isnoneornil(L, 2)){
            luaL_checktype(L, 2, LUA_TTABLE);
            mask = 0;
            const lua_Integer length = (lua_Integer)lua_rawlen(L, 2);
            for (lua_Integer i = 1; i <= length; i++){
                lua_rawgeti(L, 2, i);
                int isNumber = 0;
                const lua_Integer value = lua_tointegerx(L, -1, &isNumber);
                lua_pop(L, 1);
                if (!isNumber || value < 0 || value > static_cast<lua_Integer>(LogLevel::Fatal))
                    return luaL_argerror(L, 2, "a list of LogLevel values (LogLevel.TRACE .. LogLevel.FATAL)");
                mask |= logLevelBit(static_cast<LogLevel>(value));
            }
        }
        const std::vector<LogEntry> entries = Log::getEntries(static_cast<size_t>(maxCount), mask);
        lua_createtable(L, static_cast<int>(entries.size()), 0);
        for (size_t i = 0; i < entries.size(); i++){
            const LogEntry& entry = entries[i];
            lua_createtable(L, 0, 9);
            lua_pushinteger(L, static_cast<lua_Integer>(entry.id));
            lua_setfield(L, -2, "id");
            lua_pushstring(L, entry.category ? entry.category->name : "engine");
            lua_setfield(L, -2, "category");
            lua_pushinteger(L, static_cast<lua_Integer>(entry.level));
            lua_setfield(L, -2, "level");
            lua_pushlstring(L, entry.message.data(), entry.message.size());
            lua_setfield(L, -2, "message");
            lua_pushlstring(L, entry.stackTrace.data(), entry.stackTrace.size());
            lua_setfield(L, -2, "stackTrace");
            lua_pushnumber(L, entry.time);
            lua_setfield(L, -2, "time");
            lua_pushinteger(L, static_cast<lua_Integer>(entry.frame));
            lua_setfield(L, -2, "frame");
            lua_pushboolean(L, entry.mainThread);
            lua_setfield(L, -2, "mainThread");
            lua_pushboolean(L, entry.fromLua);
            lua_setfield(L, -2, "fromLua");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    // Log.count(level)
    int luaLogCount(lua_State* L){
        const lua_Integer level = luaL_checkinteger(L, 1);
        if (level < 0 || level > static_cast<lua_Integer>(LogLevel::Fatal))
            return luaL_argerror(L, 1, "a LogLevel value (LogLevel.TRACE .. LogLevel.FATAL)");
        lua_pushinteger(L, static_cast<lua_Integer>(Log::getCount(static_cast<LogLevel>(level))));
        return 1;
    }

    // DebugOverlay.statsMode = DebugOverlay.STATS_OFF / STATS_FPS / STATS_FULL (no other value)
    int luaDebugOverlaySetStatsMode(lua_State* L){
        // a number with an integral value (2 and 2.0), 0 .. 2
        const lua_Number mode = lua_type(L, 1) == LUA_TNUMBER ? lua_tonumber(L, 1) : -1.0;
        if (mode != std::floor(mode) || mode < static_cast<lua_Number>(DebugOverlaySettings::Stats::Off) ||
            mode > static_cast<lua_Number>(DebugOverlaySettings::Stats::Full))
            return luaL_argerror(L, 1, "DebugOverlay.statsMode takes DebugOverlay.STATS_OFF, STATS_FPS or STATS_FULL (0, 1 or 2)");
        DebugOverlay::setStatsMode(static_cast<DebugOverlaySettings::Stats>(static_cast<int>(mode)));
        return 0;
    }

    // DebugOverlay.getStats(): DebugStatsSnapshot's fields, camelCase
    int luaDebugOverlayGetStats(lua_State* L){
        const DebugStatsSnapshot s = DebugStats::sample();
        lua_createtable(L, 0, 20);
        auto number = [L](const char* name, double value){
            lua_pushnumber(L, value);
            lua_setfield(L, -2, name);
        };
        auto integer = [L](const char* name, uint64_t value){
            lua_pushinteger(L, static_cast<lua_Integer>(value));
            lua_setfield(L, -2, name);
        };
        number("fps", s.fps);
        number("frameMs", s.frameMs);
        number("frameMsAvg", s.frameMsAvg);
        number("frameMsMin", s.frameMsMin);
        number("frameMsMax", s.frameMsMax);
        number("frameMs1PercentLow", s.frameMs1PercentLow);
        integer("drawCalls", s.drawCalls);
        integer("setPassCalls", s.setPassCalls);
        integer("instances", s.instances);
        integer("triangles", s.triangles);
        integer("processMemoryBytes", s.processMemoryBytes);
        integer("luaMemoryBytes", s.luaMemoryBytes);
        integer("audioVoices", s.audioVoices);
        integer("audioVoicesMax", s.audioVoicesMax);
        integer("bodies2D", s.bodies2D);
        integer("bodies3D", s.bodies3D);
        integer("entities", s.entities);
        integer("scenes", s.scenes);
        integer("coroutines", s.coroutines);
        integer("timers", s.timers);
        integer("htmlUiDrawCalls", s.htmlUiDrawCalls);
        return 1;
    }

    // a console command's Lua function, unreferenced when the command goes (while its state lives)
    struct LuaCommandFunction {
        lua_State* L = nullptr;
        int ref = LUA_NOREF;
        ~LuaCommandFunction(){
            if (L && ref != LUA_NOREF && LuaBinding::getLuaState() == L)
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }
    };

    // the table `self` of the Lua function that called (a script method), or 0
    const void* callerSelf(lua_State* L){
        lua_Debug ar;
        if (!lua_getstack(L, 1, &ar))
            return nullptr;
        const void* self = nullptr;
        for (int i = 1; ; i++){
            const char* name = lua_getlocal(L, &ar, i);
            if (!name)
                break;
            if (std::strcmp(name, "self") == 0 && lua_istable(L, -1))
                self = lua_topointer(L, -1);
            lua_pop(L, 1);
            if (self || std::strcmp(name, "self") == 0)
                break;
        }
        return self;
    }

    // The console commands of a script go when it is released (LuaBinding::removeScriptSubscriptions:
    // SubscriptionOwnerIndex::releaseOwner): one entry per owner, its key the owner's, whose removal
    // drops the commands tagged "_<key>_"
    class DebugCommandOwners : public detail::SlotStateBase {
    public:
        bool removeSlot(uint64_t ownerKey) override {
            const std::string tag = "_" + std::to_string(ownerKey) + "_";
            const size_t before = DebugOverlay::getCommands().size();
            DebugOverlay::removeCommandsByTag(tag);
            return DebugOverlay::getCommands().size() != before;
        }
    };

    const std::shared_ptr<DebugCommandOwners>& debugCommandOwners(){
        static const std::shared_ptr<DebugCommandOwners> owners = std::make_shared<DebugCommandOwners>();
        return owners;
    }

    // DebugOverlay.addCommand(name, description, fn [, owner]): fn(args) gets the words after the
    // name. The owner (a script table; by default the `self` of the calling method) takes the
    // command with it when it is released
    int luaDebugOverlayAddCommand(lua_State* L){
        size_t nameLength = 0;
        const char* name = luaL_checklstring(L, 1, &nameLength);
        size_t descriptionLength = 0;
        const char* description = lua_isnoneornil(L, 2) ? "" : luaL_checklstring(L, 2, &descriptionLength);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        const void* owner = nullptr;
        if (!lua_isnoneornil(L, 4)){
            luaL_checktype(L, 4, LUA_TTABLE);
            owner = lua_topointer(L, 4);
        }else{
            owner = callerSelf(L);
        }
        lua_pushvalue(L, 3);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        bool added = false;
        {
            const std::string commandName(name, nameLength);
            auto function = std::make_shared<LuaCommandFunction>();
            function->L = L;
            function->ref = ref;
            // without an owner a Lua command still goes with the Lua state (Play stop, quit)
            const std::string ownerTag = owner ? "_" + std::to_string(reinterpret_cast<std::uintptr_t>(owner)) + "_" : std::string("lua");
            added = DebugOverlay::addCommand(commandName, std::string(description, descriptionLength),
                [function, commandName](const std::vector<std::string>& args){
                    lua_State* state = function->L;
                    if (!state || LuaBinding::getLuaState() != state)
                        return;
                    lua_rawgeti(state, LUA_REGISTRYINDEX, function->ref);
                    lua_createtable(state, static_cast<int>(args.size()), 0);
                    for (size_t i = 0; i < args.size(); i++){
                        lua_pushlstring(state, args[i].data(), args[i].size());
                        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                    }
                    if (LuaBinding::pcallWithTraceback(state, 1, 0) != LUA_OK){
                        const std::string error = LuaBinding::getLuaStackErrorString(state, -1);
                        lua_pop(state, 1);
                        Log::error("Lua Error: console command '%s': %s", commandName.c_str(), error.c_str());
                    }
                }, ownerTag);
            if (added && owner)
                SubscriptionOwnerIndex::add(ownerKeyOf(owner), debugCommandOwners(), ownerKeyOf(owner));
        }
        lua_pushboolean(L, added);
        return 1;
    }

}

void LuaBinding::registerCoreClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("Scaling")
        .addVariable("FITWIDTH", Scaling::FITWIDTH)
        .addVariable("FITHEIGHT", Scaling::FITHEIGHT)
        .addVariable("LETTERBOX", Scaling::LETTERBOX)
        .addVariable("CROP", Scaling::CROP)
        .addVariable("STRETCH", Scaling::STRETCH)
        .addVariable("NATIVE", Scaling::NATIVE)
        .addVariable("INTEGER", Scaling::INTEGER)
        .addVariable("INTEGER_EXPAND", Scaling::INTEGER_EXPAND)
        .endNamespace();

    // the Pixel Perfect Camera's choices (S41)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PixelUpscaleMode")
        .addVariable("INTEGER", PixelUpscaleMode::INTEGER)
        .addVariable("INTEGER_EXPAND", PixelUpscaleMode::INTEGER_EXPAND)
        .addVariable("STRETCH_FILL", PixelUpscaleMode::STRETCH_FILL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PixelRotation")
        .addVariable("CAMERA", PixelRotation::CAMERA)
        .addVariable("STEPS_90", PixelRotation::STEPS_90)
        .addVariable("ON_GRID", PixelRotation::ON_GRID)
        .endNamespace();

    // the Canvas Scaler (Unity's CanvasScaler.ScaleMode, ScreenMatchMode, Unit)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("UIScaleMode")
        .addVariable("FollowGameCanvas", UIScaleMode::FollowGameCanvas)
        .addVariable("ConstantPixelSize", UIScaleMode::ConstantPixelSize)
        .addVariable("ScaleWithScreenSize", UIScaleMode::ScaleWithScreenSize)
        .addVariable("ConstantPhysicalSize", UIScaleMode::ConstantPhysicalSize)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ScreenMatchMode")
        .addVariable("MatchWidthOrHeight", ScreenMatchMode::MatchWidthOrHeight)
        .addVariable("Expand", ScreenMatchMode::Expand)
        .addVariable("Shrink", ScreenMatchMode::Shrink)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PhysicalUnit")
        .addVariable("Centimeters", PhysicalUnit::Centimeters)
        .addVariable("Millimeters", PhysicalUnit::Millimeters)
        .addVariable("Inches", PhysicalUnit::Inches)
        .addVariable("Points", PhysicalUnit::Points)
        .addVariable("Picas", PhysicalUnit::Picas)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<CanvasScalerRef>("CanvasScaler")
        .addProperty("uiScaleMode", &CanvasScalerRef::getUIScaleMode, &CanvasScalerRef::setUIScaleMode)
        .addProperty("scaleFactor", &CanvasScalerRef::getScaleFactor, &CanvasScalerRef::setScaleFactor)
        .addProperty("referenceResolution", &CanvasScalerRef::getReferenceResolution, &CanvasScalerRef::setReferenceResolution)
        .addProperty("screenMatchMode", &CanvasScalerRef::getScreenMatchMode, &CanvasScalerRef::setScreenMatchMode)
        .addProperty("matchWidthOrHeight", &CanvasScalerRef::getMatchWidthOrHeight, &CanvasScalerRef::setMatchWidthOrHeight)
        .addProperty("physicalUnit", &CanvasScalerRef::getPhysicalUnit, &CanvasScalerRef::setPhysicalUnit)
        .addProperty("fallbackScreenDPI", &CanvasScalerRef::getFallbackScreenDPI, &CanvasScalerRef::setFallbackScreenDPI)
        .addProperty("useProjectDefault", &CanvasScalerRef::isUseProjectDefault, &CanvasScalerRef::setUseProjectDefault)
        .addProperty("canvasSize", &CanvasScalerRef::getCanvasSize)
        .addProperty("appliedScaleFactor", &CanvasScalerRef::getAppliedScaleFactor)
        .endClass();

    // the UI's statics: canvas pixels (Input, the game canvas) <-> a scene's UI units
    luabridge::getGlobalNamespace(L)
        .beginClass<UIStatics>("UI")
        .addStaticFunction("canvasToUI", &UIStatics::canvasToUI)
        .addStaticFunction("uiToCanvas", &UIStatics::uiToCanvas)
        .addStaticProperty("reducedMotion", &UISystem::isReducedMotion, &UISystem::setReducedMotion)
        // ui-components: the scene's selection (Unity's EventSystem.currentSelectedGameObject):
        // a GameObject or nil
        .addStaticFunction("getSelected", +[](Scene* scene) -> std::optional<GameObject> {
            if (!scene) return std::nullopt;
            const Entity entity = scene->getSystem<UISystem>()->getSelected();
            if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return std::nullopt;
            return GameObject(scene, entity);
        })
        // Unity's SetSelectedGameObject: a Button (GameObject or entity), nil to deselect; false
        // when it cannot be selected (not a Button, inactive, hidden, an on-screen control)
        .addStaticFunction("setSelected", +[](Scene* scene, luabridge::LuaRef value) -> bool {
            if (!scene) return false;
            Entity entity = NULL_ENTITY;
            if (value.isNumber()){
                entity = (Entity)value.unsafe_cast<lua_Integer>();
            }else if (!value.isNil()){
                lua_State* Lv = value.state();
                value.push(Lv);
                auto handle = luabridge::get<EntityHandle*>(Lv, -1);
                lua_pop(Lv, 1);
                if (!handle || !handle.value()) return false;
                entity = handle.value()->getEntity();
            }
            return scene->getSystem<UISystem>()->setSelected(entity);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("Platform")
        .addVariable("MacOS", Platform::MacOS)
        .addVariable("iOS", Platform::iOS)
        .addVariable("Web", Platform::Web)
        .addVariable("Android", Platform::Android)
        .addVariable("Linux", Platform::Linux)
        .addVariable("Windows", Platform::Windows)
        .endNamespace();

    // Unity's SystemLanguage (System.getSystemLanguage), same names and values
    luabridge::getGlobalNamespace(L)
        .beginNamespace("SystemLanguage")
        .addVariable("Afrikaans", SystemLanguage::Afrikaans)
        .addVariable("Arabic", SystemLanguage::Arabic)
        .addVariable("Basque", SystemLanguage::Basque)
        .addVariable("Belarusian", SystemLanguage::Belarusian)
        .addVariable("Bulgarian", SystemLanguage::Bulgarian)
        .addVariable("Catalan", SystemLanguage::Catalan)
        .addVariable("Chinese", SystemLanguage::Chinese)
        .addVariable("Czech", SystemLanguage::Czech)
        .addVariable("Danish", SystemLanguage::Danish)
        .addVariable("Dutch", SystemLanguage::Dutch)
        .addVariable("English", SystemLanguage::English)
        .addVariable("Estonian", SystemLanguage::Estonian)
        .addVariable("Faroese", SystemLanguage::Faroese)
        .addVariable("Finnish", SystemLanguage::Finnish)
        .addVariable("French", SystemLanguage::French)
        .addVariable("German", SystemLanguage::German)
        .addVariable("Greek", SystemLanguage::Greek)
        .addVariable("Hebrew", SystemLanguage::Hebrew)
        .addVariable("Hungarian", SystemLanguage::Hungarian)
        .addVariable("Icelandic", SystemLanguage::Icelandic)
        .addVariable("Indonesian", SystemLanguage::Indonesian)
        .addVariable("Italian", SystemLanguage::Italian)
        .addVariable("Japanese", SystemLanguage::Japanese)
        .addVariable("Korean", SystemLanguage::Korean)
        .addVariable("Latvian", SystemLanguage::Latvian)
        .addVariable("Lithuanian", SystemLanguage::Lithuanian)
        .addVariable("Norwegian", SystemLanguage::Norwegian)
        .addVariable("Polish", SystemLanguage::Polish)
        .addVariable("Portuguese", SystemLanguage::Portuguese)
        .addVariable("Romanian", SystemLanguage::Romanian)
        .addVariable("Russian", SystemLanguage::Russian)
        .addVariable("SerboCroatian", SystemLanguage::SerboCroatian)
        .addVariable("Slovak", SystemLanguage::Slovak)
        .addVariable("Slovenian", SystemLanguage::Slovenian)
        .addVariable("Spanish", SystemLanguage::Spanish)
        .addVariable("Swedish", SystemLanguage::Swedish)
        .addVariable("Thai", SystemLanguage::Thai)
        .addVariable("Turkish", SystemLanguage::Turkish)
        .addVariable("Ukrainian", SystemLanguage::Ukrainian)
        .addVariable("Vietnamese", SystemLanguage::Vietnamese)
        .addVariable("ChineseSimplified", SystemLanguage::ChineseSimplified)
        .addVariable("ChineseTraditional", SystemLanguage::ChineseTraditional)
        .addVariable("Unknown", SystemLanguage::Unknown)
        .addVariable("Hindi", SystemLanguage::Hindi)
        .endNamespace();

    // Unity's ScreenOrientation (Screen.orientation)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ScreenOrientation")
        .addVariable("Portrait", ScreenOrientation::Portrait)
        .addVariable("PortraitUpsideDown", ScreenOrientation::PortraitUpsideDown)
        .addVariable("LandscapeLeft", ScreenOrientation::LandscapeLeft)
        .addVariable("LandscapeRight", ScreenOrientation::LandscapeRight)
        .addVariable("AutoRotation", ScreenOrientation::AutoRotation)
        .endNamespace();

    // Unity's DeviceOrientation (Input.deviceOrientation)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("DeviceOrientation")
        .addVariable("Unknown", DeviceOrientation::Unknown)
        .addVariable("Portrait", DeviceOrientation::Portrait)
        .addVariable("PortraitUpsideDown", DeviceOrientation::PortraitUpsideDown)
        .addVariable("LandscapeLeft", DeviceOrientation::LandscapeLeft)
        .addVariable("LandscapeRight", DeviceOrientation::LandscapeRight)
        .addVariable("FaceUp", DeviceOrientation::FaceUp)
        .addVariable("FaceDown", DeviceOrientation::FaceDown)
        .endNamespace();

    // the quality tier a device gets (Device.tier, Device.setTier; docs/device-tiers.md)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("DeviceTier")
        .addVariable("Low", DeviceTier::Low)
        .addVariable("Medium", DeviceTier::Medium)
        .addVariable("High", DeviceTier::High)
        .addVariable("Ultra", DeviceTier::Ultra)
        .endNamespace();

    // the platform's thermal state (Device.thermalState, Device.onThermalStateChanged)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ThermalState")
        .addVariable("Nominal", ThermalState::Nominal)
        .addVariable("Fair", ThermalState::Fair)
        .addVariable("Serious", ThermalState::Serious)
        .addVariable("Critical", ThermalState::Critical)
        .endNamespace();

    // Unity's DeviceType (System.getDeviceType)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("DeviceType")
        .addVariable("Unknown", DeviceType::Unknown)
        .addVariable("Handheld", DeviceType::Handheld)
        .addVariable("Console", DeviceType::Console)
        .addVariable("Desktop", DeviceType::Desktop)
        .endNamespace();

    // Unity's NetworkReachability (System.getInternetReachability)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("NetworkReachability")
        .addVariable("NotReachable", NetworkReachability::NotReachable)
        .addVariable("ReachableViaCarrierDataNetwork", NetworkReachability::ReachableViaCarrierDataNetwork)
        .addVariable("ReachableViaLocalAreaNetwork", NetworkReachability::ReachableViaLocalAreaNetwork)
        .endNamespace();

    // System.hapticFeedback kinds
    luabridge::getGlobalNamespace(L)
        .beginNamespace("HapticFeedback")
        .addVariable("Light", HapticFeedback::Light)
        .addVariable("Medium", HapticFeedback::Medium)
        .addVariable("Heavy", HapticFeedback::Heavy)
        .addVariable("Selection", HapticFeedback::Selection)
        .addVariable("Success", HapticFeedback::NotificationSuccess)
        .addVariable("Warning", HapticFeedback::NotificationWarning)
        .addVariable("Error", HapticFeedback::NotificationError)
        .endNamespace();

    // Unity's SleepTimeout (Screen.sleepTimeout)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("SleepTimeout")
        .addVariable("NeverSleep", SleepTimeout::NeverSleep)
        .addVariable("SystemSetting", SleepTimeout::SystemSetting)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("GraphicBackend")
        .addVariable("GLCORE", GraphicBackend::GLCORE)
        .addVariable("GLES3", GraphicBackend::GLES3)
        .addVariable("D3D11", GraphicBackend::D3D11)
        .addVariable("METAL", GraphicBackend::METAL)
        .addVariable("WGPU", GraphicBackend::WGPU)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureStrategy")
        .addVariable("FIT", TextureStrategy::FIT)
        .addVariable("RESIZE", TextureStrategy::RESIZE)
        .addVariable("NONE", TextureStrategy::NONE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureType")
        .addVariable("TEXTURE_2D", TextureType::TEXTURE_2D)
        .addVariable("TEXTURE_3D", TextureType::TEXTURE_3D)
        .addVariable("TEXTURE_CUBE", TextureType::TEXTURE_CUBE)
        .addVariable("TEXTURE_ARRAY", TextureType::TEXTURE_ARRAY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ColorFormat")
        .addVariable("RED", ColorFormat::RED)
        .addVariable("RGBA", ColorFormat::RGBA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PrimitiveType")
        .addVariable("TRIANGLES", PrimitiveType::TRIANGLES)
        .addVariable("TRIANGLE_STRIP", PrimitiveType::TRIANGLE_STRIP)
        .addVariable("POINTS", PrimitiveType::POINTS)
        .addVariable("LINES", PrimitiveType::LINES)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CullingMode")
        .addVariable("BACK", CullingMode::BACK)
        .addVariable("FRONT", CullingMode::FRONT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("WindingOrder")
        .addVariable("CCW", WindingOrder::CCW)
        .addVariable("CW", WindingOrder::CW)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LodFadeMode")
        .addVariable("NONE", LodFadeMode::NONE)
        .addVariable("CROSS_FADE", LodFadeMode::CROSS_FADE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialAlphaMode")
        .addVariable("AUTO", MaterialAlphaMode::AUTO)
        .addVariable("ALPHA_OPAQUE", MaterialAlphaMode::ALPHA_OPAQUE)
        .addVariable("MASK", MaterialAlphaMode::MASK)
        .addVariable("BLEND", MaterialAlphaMode::BLEND)
        .endNamespace();

    // shader library render state of a Material (and UI/points/lines blendMode, depthWrite)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialBlendMode")
        .addVariable("AUTO", MaterialBlendMode::AUTO)
        .addVariable("NONE", MaterialBlendMode::NONE)
        .addVariable("ALPHA", MaterialBlendMode::ALPHA)
        .addVariable("PREMULTIPLIED", MaterialBlendMode::PREMULTIPLIED)
        .addVariable("ADDITIVE", MaterialBlendMode::ADDITIVE)
        .addVariable("MULTIPLY", MaterialBlendMode::MULTIPLY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialCullMode")
        .addVariable("AUTO", MaterialCullMode::AUTO)
        .addVariable("BACK", MaterialCullMode::BACK)
        .addVariable("FRONT", MaterialCullMode::FRONT)
        .addVariable("OFF", MaterialCullMode::OFF)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialDepthWrite")
        .addVariable("AUTO", MaterialDepthWrite::AUTO)
        .addVariable("ENABLED", MaterialDepthWrite::ENABLED)
        .addVariable("DISABLED", MaterialDepthWrite::DISABLED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialDepthTest")
        .addVariable("AUTO", MaterialDepthTest::AUTO)
        .addVariable("LESS", MaterialDepthTest::LESS)
        .addVariable("LESS_EQUAL", MaterialDepthTest::LESS_EQUAL)
        .addVariable("EQUAL", MaterialDepthTest::EQUAL)
        .addVariable("GREATER_EQUAL", MaterialDepthTest::GREATER_EQUAL)
        .addVariable("GREATER", MaterialDepthTest::GREATER)
        .addVariable("NOT_EQUAL", MaterialDepthTest::NOT_EQUAL)
        .addVariable("ALWAYS", MaterialDepthTest::ALWAYS)
        .endNamespace();

    // stencil test of a Material (and UI/points/lines): AUTO takes the shader's @state,
    // DISABLED turns it off, any other compare makes the whole MaterialStencil explicit
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialStencilCompare")
        .addVariable("AUTO", MaterialStencilCompare::AUTO)
        .addVariable("DISABLED", MaterialStencilCompare::DISABLED)
        .addVariable("NEVER", MaterialStencilCompare::NEVER)
        .addVariable("LESS", MaterialStencilCompare::LESS)
        .addVariable("EQUAL", MaterialStencilCompare::EQUAL)
        .addVariable("LESS_EQUAL", MaterialStencilCompare::LESS_EQUAL)
        .addVariable("GREATER", MaterialStencilCompare::GREATER)
        .addVariable("NOT_EQUAL", MaterialStencilCompare::NOT_EQUAL)
        .addVariable("GREATER_EQUAL", MaterialStencilCompare::GREATER_EQUAL)
        .addVariable("ALWAYS", MaterialStencilCompare::ALWAYS)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialStencilOp")
        .addVariable("KEEP", MaterialStencilOp::KEEP)
        .addVariable("ZERO", MaterialStencilOp::ZERO)
        .addVariable("REPLACE", MaterialStencilOp::REPLACE)
        .addVariable("INCR_CLAMP", MaterialStencilOp::INCR_CLAMP)
        .addVariable("DECR_CLAMP", MaterialStencilOp::DECR_CLAMP)
        .addVariable("INVERT", MaterialStencilOp::INVERT)
        .addVariable("INCR_WRAP", MaterialStencilOp::INCR_WRAP)
        .addVariable("DECR_WRAP", MaterialStencilOp::DECR_WRAP)
        .endNamespace();

    // colorWrite values: AUTO (-1), NONE (0) or a sum of R, G, B, A
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialColorWrite")
        .addVariable("AUTO", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_AUTO)
        .addVariable("NONE", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_NONE)
        .addVariable("R", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_R)
        .addVariable("G", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_G)
        .addVariable("B", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_B)
        .addVariable("A", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_A)
        .addVariable("RGB", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_RGB)
        .addVariable("ALL", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_ALL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialLighting")
        .addVariable("AUTO", MaterialLighting::AUTO)
        .addVariable("PIXEL", MaterialLighting::PIXEL)
        .addVariable("VERTEX", MaterialLighting::VERTEX)
        .addVariable("MIXED", MaterialLighting::MIXED)
        .addVariable("NONE", MaterialLighting::NONE)
        .endNamespace();

    // shader library phase 2: Material.extraPass.order
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialPassOrder")
        .addVariable("AFTER", MaterialPassOrder::AFTER)
        .addVariable("BEFORE", MaterialPassOrder::BEFORE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureFilter")
        .addVariable("NEAREST", TextureFilter::NEAREST)
        .addVariable("LINEAR", TextureFilter::LINEAR)
        .addVariable("NEAREST_MIPMAP_NEAREST", TextureFilter::NEAREST_MIPMAP_NEAREST)
        .addVariable("NEAREST_MIPMAP_LINEAR", TextureFilter::NEAREST_MIPMAP_LINEAR)
        .addVariable("LINEAR_MIPMAP_NEAREST", TextureFilter::LINEAR_MIPMAP_NEAREST)
        .addVariable("LINEAR_MIPMAP_LINEAR", TextureFilter::LINEAR_MIPMAP_LINEAR)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureWrap")
        .addVariable("REPEAT", TextureWrap::REPEAT)
        .addVariable("MIRRORED_REPEAT", TextureWrap::MIRRORED_REPEAT)
        .addVariable("CLAMP_TO_EDGE", TextureWrap::CLAMP_TO_EDGE)
        .addVariable("CLAMP_TO_BORDER", TextureWrap::CLAMP_TO_BORDER)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureAlphaBorder")
        .addVariable("AUTO", TextureAlphaBorder::AUTO)
        .addVariable("FIX", TextureAlphaBorder::FIX)
        .addVariable("KEEP", TextureAlphaBorder::KEEP)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CursorType")
        .addVariable("ARROW", CursorType::ARROW)
        .addVariable("IBEAM", CursorType::IBEAM)
        .addVariable("CROSSHAIR", CursorType::CROSSHAIR)
        .addVariable("POINTING_HAND", CursorType::POINTING_HAND)
        .addVariable("RESIZE_EW", CursorType::RESIZE_EW)
        .addVariable("RESIZE_NS", CursorType::RESIZE_NS)
        .addVariable("RESIZE_NWSE", CursorType::RESIZE_NWSE)
        .addVariable("RESIZE_NESW", CursorType::RESIZE_NESW)
        .addVariable("RESIZE_ALL", CursorType::RESIZE_ALL)
        .addVariable("NOT_ALLOWED", CursorType::NOT_ALLOWED)
        .addVariable("WAIT", CursorType::WAIT)
        .addVariable("PROGRESS", CursorType::PROGRESS)
        .addVariable("HELP", CursorType::HELP)
        .addVariable("GRAB", CursorType::GRAB)
        .addVariable("GRABBING", CursorType::GRABBING)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MouseMode")
        .addVariable("NORMAL", MouseMode::NORMAL)
        .addVariable("HIDDEN", MouseMode::HIDDEN)
        .addVariable("CAPTURED", MouseMode::CAPTURED)
        .addVariable("CONFINED", MouseMode::CONFINED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ResourceLoadState")
        .addVariable("NotStarted", ResourceLoadState::NotStarted)
        .addVariable("Loading", ResourceLoadState::Loading)
        .addVariable("Finished", ResourceLoadState::Finished)
        .addVariable("Failed", ResourceLoadState::Failed)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightState")
        .addVariable("OFF", LightState::OFF)
        .addVariable("ON", LightState::ON)
        .addVariable("AUTO", LightState::AUTO)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowQuality")
        .addVariable("NONE", ShadowQuality::NONE)
        .addVariable("LOW", ShadowQuality::LOW)
        .addVariable("MEDIUM", ShadowQuality::MEDIUM)
        .addVariable("HIGH", ShadowQuality::HIGH)
        .addVariable("AUTO", ShadowQuality::AUTO)
        .addVariable("ULTRA", ShadowQuality::ULTRA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowType")
        .addVariable("NONE", ShadowType::NONE)
        .addVariable("HARD", ShadowType::HARD)
        .addVariable("SOFT", ShadowType::SOFT)
        .endNamespace();

    // S57's lighting
    luabridge::getGlobalNamespace(L)
        .beginNamespace("TransparencyMode")
        .addVariable("SORTED", TransparencyMode::SORTED)
        .addVariable("ORDER_INDEPENDENT", TransparencyMode::ORDER_INDEPENDENT)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadingModel")
        .addVariable("STANDARD", ShadingModel::STANDARD)
        .addVariable("HAIR", ShadingModel::HAIR)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("CapsuleCastShadows")
        .addVariable("ON", CapsuleCastShadows::ON)
        .addVariable("CAPSULES_ONLY", CapsuleCastShadows::CAPSULES_ONLY)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("CapsuleDirection")
        .addVariable("X_AXIS", CapsuleDirection::X_AXIS)
        .addVariable("Y_AXIS", CapsuleDirection::Y_AXIS)
        .addVariable("Z_AXIS", CapsuleDirection::Z_AXIS)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightMode")
        .addVariable("AUTOMATIC", LightMode::AUTOMATIC)
        .addVariable("PER_OBJECT", LightMode::PER_OBJECT)
        .addVariable("TILED", LightMode::TILED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowUpdateMode")
        .addVariable("AUTO", ShadowUpdateMode::AUTO)
        .addVariable("EVERY_FRAME", ShadowUpdateMode::EVERY_FRAME)
        .addVariable("ON_DEMAND", ShadowUpdateMode::ON_DEMAND)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CascadeSplitMode")
        .addVariable("AUTO", CascadeSplitMode::AUTO)
        .addVariable("MANUAL", CascadeSplitMode::MANUAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowTier")
        .addVariable("LOW", ShadowTier::LOW)
        .addVariable("MEDIUM", ShadowTier::MEDIUM)
        .addVariable("HIGH", ShadowTier::HIGH)
        .addVariable("ULTRA", ShadowTier::ULTRA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LitQuality")
        .addVariable("STANDARD", LitQuality::STANDARD)
        .addVariable("HIGH", LitQuality::HIGH)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SceneColorScale")
        .addVariable("FULL", SceneColorScale::FULL)
        .addVariable("HALF", SceneColorScale::HALF)
        .endNamespace();

    // the resolution of the water reflection passes (Scene.planarReflectionResolution)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PlanarReflectionResolution")
        .addVariable("OFF", PlanarReflectionResolution::OFF)
        .addVariable("QUARTER", PlanarReflectionResolution::QUARTER)
        .addVariable("HALF", PlanarReflectionResolution::HALF)
        .addVariable("FULL", PlanarReflectionResolution::FULL)
        .endNamespace();

    // the side of the Water Wakes' map (Scene.waterWakeResolution): AUTO 256, 128 on mobile and web
    luabridge::getGlobalNamespace(L)
        .beginNamespace("WaterWakeResolution")
        .addVariable("AUTO", WaterWakeResolution::AUTO)
        .addVariable("LOW", WaterWakeResolution::LOW)
        .addVariable("MEDIUM", WaterWakeResolution::MEDIUM)
        .addVariable("HIGH", WaterWakeResolution::HIGH)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TonemapMode")
        .addVariable("NONE", TonemapMode::NONE)
        .addVariable("ACES", TonemapMode::ACES)
        .addVariable("NEUTRAL", TonemapMode::NEUTRAL)
        .addVariable("REINHARD", TonemapMode::REINHARD)
        .addVariable("AGX", TonemapMode::AGX)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MonochromeMode")
        .addVariable("GRAYSCALE", MonochromeMode::GRAYSCALE)
        .addVariable("SEPIA", MonochromeMode::SEPIA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FilmGrainType")
        .addVariable("CLASSIC", FilmGrainType::CLASSIC)
        .addVariable("LENS", FilmGrainType::LENS)
        .addVariable("FILMIC", FilmGrainType::FILMIC)
        .endNamespace();

    // Palette Quantize (S43)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PaletteDither")
        .addVariable("NONE", PaletteDither::NONE)
        .addVariable("BAYER2", PaletteDither::BAYER2)
        .addVariable("BAYER4", PaletteDither::BAYER4)
        .addVariable("BAYER8", PaletteDither::BAYER8)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PaletteDistance")
        .addVariable("OKLAB", PaletteDistance::OKLAB)
        .addVariable("RGB", PaletteDistance::RGB)
        .endNamespace();
    luabridge::getGlobalNamespace(L)
        .beginNamespace("PalettePrecision")
        .addVariable("NORMAL", PalettePrecision::NORMAL)
        .addVariable("HIGH", PalettePrecision::HIGH)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FilmGateMask")
        .addVariable("NONE", FilmGateMask::NONE)
        .addVariable("ROUNDED", FilmGateMask::ROUNDED)
        .addVariable("ACADEMY_137", FilmGateMask::ACADEMY_137)
        .addVariable("WIDESCREEN_185", FilmGateMask::WIDESCREEN_185)
        .addVariable("ANAMORPHIC_239", FilmGateMask::ANAMORPHIC_239)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LutInputEncoding")
        .addVariable("AUTO", LutInputEncoding::AUTO)
        .addVariable("SRGB", LutInputEncoding::SRGB)
        .addVariable("LINEAR", LutInputEncoding::LINEAR)
        .addVariable("LOGC3", LutInputEncoding::LOGC3)
        .addVariable("ACESCCT", LutInputEncoding::ACESCCT)
        .addVariable("CINEON", LutInputEncoding::CINEON)
        .addVariable("DAVINCI_INTERMEDIATE", LutInputEncoding::DAVINCI_INTERMEDIATE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("UIEventState")
        .addVariable("NOT_SET", UIEventState::NOT_SET)
        .addVariable("ENABLED", UIEventState::ENABLED)
        .addVariable("DISABLED", UIEventState::DISABLED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("EntityPool")
        .addVariable("User", EntityPool::User)
        .addVariable("System", EntityPool::System)
        .endNamespace();

    // Unity's LayerMask: layer names from Project Settings > Tags and Layers
    luabridge::getGlobalNamespace(L)
        .beginClass<LayerMask>("LayerMask")
        .addStaticFunction("nameToLayer", &TagManager::nameToLayer)
        .addStaticFunction("layerToName", &TagManager::layerToName)
        .addStaticFunction("getMask", &luaLayerMaskGetMask)
        .endClass();

    // Unity's Physics and Physics2D: Layer Collision Matrix (Project Settings > Physics and
    // Physics 2D) and layer-masked ray casts. Layers are numbers 0..31 (LayerMask.nameToLayer).
    luabridge::getGlobalNamespace(L)
        .beginClass<Physics>("Physics")
        .addStaticProperty("ALL_LAYERS", &luaAllLayers)
        .addStaticProperty("DEFAULT_RAYCAST_LAYERS", &luaDefaultRaycastLayers)
        .addStaticProperty("IGNORE_RAYCAST_LAYER", &luaIgnoreRaycastLayer)
        // the runtime collider and joint wireframes of every scene (3D and 2D), and the contacts
        .addStaticProperty("debugDraw", &Physics::isDebugDraw, &Physics::setDebugDraw)
        .addStaticProperty("debugDrawContacts", &Physics::isDebugDrawContacts, &Physics::setDebugDrawContacts)
        .addStaticFunction("ignoreLayerCollision", &luaIgnoreLayerCollision<Physics>)
        .addStaticFunction("getIgnoreLayerCollision", &luaGetIgnoreLayerCollision<Physics>)
        .addStaticFunction("getLayerCollisionMask", &luaGetLayerCollisionMask<Physics>)
        .addStaticFunction("setLayerCollisionMask", &luaSetLayerCollisionMask<Physics>)
        .addStaticFunction("resetLayerCollisionMatrix", &luaResetLayerCollisionMatrix<Physics>)
        .addStaticFunction("ignoreCollision", &luaIgnoreCollision<false>)
        .addStaticFunction("getIgnoreCollision", &luaGetIgnoreCollision<false>)
#ifdef JASPERENGINE_PHYSICS_3D
        .addStaticFunction("raycast", &luaRaycast<Vector3, false, false>)
        .addStaticFunction("raycastAll", &luaRaycast<Vector3, true, false>)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics2D>("Physics2D")
        .addStaticProperty("ALL_LAYERS", &luaAllLayers)
        .addStaticProperty("DEFAULT_RAYCAST_LAYERS", &luaDefaultRaycastLayers)
        .addStaticProperty("IGNORE_RAYCAST_LAYER", &luaIgnoreRaycastLayer)
        .addStaticFunction("ignoreLayerCollision", &luaIgnoreLayerCollision<Physics2D>)
        .addStaticFunction("getIgnoreLayerCollision", &luaGetIgnoreLayerCollision<Physics2D>)
        .addStaticFunction("getLayerCollisionMask", &luaGetLayerCollisionMask<Physics2D>)
        .addStaticFunction("setLayerCollisionMask", &luaSetLayerCollisionMask<Physics2D>)
        .addStaticFunction("resetLayerCollisionMatrix", &luaResetLayerCollisionMatrix<Physics2D>)
        .addStaticFunction("ignoreCollision", &luaIgnoreCollision<true>)
        .addStaticFunction("getIgnoreCollision", &luaGetIgnoreCollision<true>)
#ifdef JASPERENGINE_PHYSICS_2D
        .addStaticFunction("raycast", &luaRaycast<Vector2, false, true>)
        .addStaticFunction("raycastAll", &luaRaycast<Vector2, true, true>)
#endif
        .endClass();

    // MeshRaycast: ray casts against what is drawn (triangles, terrains, UI rects), no physics
    luabridge::getGlobalNamespace(L)
        .beginClass<MeshRaycastHit>("MeshRaycastHit")
        .addProperty("hit", &MeshRaycastHit::hit)
        .addProperty("distance", &MeshRaycastHit::distance)
        .addProperty("point", &MeshRaycastHit::point)
        .addProperty("normal", &MeshRaycastHit::normal)
        .addProperty("faceNormal", &MeshRaycastHit::faceNormal)
        .addProperty("barycentric", &MeshRaycastHit::barycentric)
        .addProperty("pending", &MeshRaycastHit::pending)
        .addProperty("entity", &luaMeshRaycastHitEntity)
        .addProperty("submesh", &luaMeshRaycastHitSubmesh)
        .addProperty("triangle", &luaMeshRaycastHitTriangle)
        .addProperty("instance", &luaMeshRaycastHitInstance)
        .addProperty("kind", &luaMeshRaycastHitKind)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<MeshRaycast>("MeshRaycast")
        .addStaticProperty("MESH", &luaMeshRaycastMesh)
        .addStaticProperty("INSTANCE", &luaMeshRaycastInstance)
        .addStaticProperty("TERRAIN", &luaMeshRaycastTerrain)
        .addStaticProperty("UI", &luaMeshRaycastUI)
        .addStaticProperty("BOUNDS", &luaMeshRaycastBounds)
        .addStaticProperty("ALL", &luaMeshRaycastAll)
        .addStaticFunction("raycast", &luaMeshRaycast<false>)
        .addStaticFunction("raycastAll", &luaMeshRaycast<true>)
        .addStaticFunction("raycastEntity", &luaMeshRaycastEntityHit)
        .addStaticFunction("prepare", &luaMeshRaycastPrepare)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShaderType")
        .addVariable("POINTS", ShaderType::POINTS)
        .addVariable("LINES", ShaderType::LINES)
        .addVariable("MESH", ShaderType::MESH)
        .addVariable("SKYBOX", ShaderType::SKYBOX)
        .addVariable("DEPTH", ShaderType::DEPTH)
        .addVariable("GBUFFER", ShaderType::GBUFFER)
        .addVariable("UI", ShaderType::UI)
        .addVariable("SSAO", ShaderType::SSAO)
        .addVariable("SSAO_BLUR", ShaderType::SSAO_BLUR)
        .addVariable("SSR", ShaderType::SSR)
        .addVariable("SSR_BLUR", ShaderType::SSR_BLUR)
        .addVariable("COMPOSITE", ShaderType::COMPOSITE)
        .addVariable("SHADOW2D", ShaderType::SHADOW2D)
        .addVariable("BLIT", ShaderType::BLIT)
        .addVariable("POSTPROCESS", ShaderType::POSTPROCESS)
        .addVariable("BLOOM", ShaderType::BLOOM)
        .addVariable("HTMLUI", ShaderType::HTMLUI)
        .addVariable("HTMLUI_GRADIENT", ShaderType::HTMLUI_GRADIENT)
        .addVariable("HTMLUI_PRIM", ShaderType::HTMLUI_PRIM)
        .addVariable("HTMLUI_POST", ShaderType::HTMLUI_POST)
        .addVariable("UI_EFFECT", ShaderType::UI_EFFECT)
        .addVariable("PROBE_FILTER", ShaderType::PROBE_FILTER)
        .addVariable("FLUID", ShaderType::FLUID)
        .addVariable("ATMOSPHERE_DEPTH", ShaderType::ATMOSPHERE_DEPTH)
        .addVariable("VOLUMETRIC_FOG", ShaderType::VOLUMETRIC_FOG)
        .addVariable("ATMOSPHERE_DENOISE", ShaderType::ATMOSPHERE_DENOISE)
        .addVariable("VOLUMETRIC_CLOUDS", ShaderType::VOLUMETRIC_CLOUDS)
        .addVariable("SKY_COMPOSITE", ShaderType::SKY_COMPOSITE)
        .addVariable("VOLUMETRIC_COMPOSITE", ShaderType::VOLUMETRIC_COMPOSITE)
        .addVariable("PASS_PREVIEW", ShaderType::PASS_PREVIEW)
        .endNamespace();

    // Shader variants by spec ("mesh:Uv1,Vc4"), or by type and property bits; an
    // optional last argument names a forked shader (its project-relative base path).
    // The property bits are a Lua integer taken as the 64-bit mask it holds (1 << 63
    // is negative in Lua)
    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderPool>("ShaderPool")
        .addStaticFunction("warmUp",
            luabridge::overload<const std::string&>(+[](const std::string& spec) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key) && ShaderPool::warmUp(key);
            }),
            luabridge::overload<const std::string&, const std::string&>(+[](const std::string& spec, const std::string& customShader) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key, nullptr, customShader) && ShaderPool::warmUp(key);
            }),
            luabridge::overload<ShaderType, lua_Integer>(+[](ShaderType type, lua_Integer properties) {
                return ShaderPool::warmUp(type, static_cast<uint64_t>(properties));
            }),
            luabridge::overload<ShaderType, lua_Integer, const std::string&>(+[](ShaderType type, lua_Integer properties, const std::string& customShader) {
                return ShaderPool::warmUp(type, static_cast<uint64_t>(properties), ShaderPool::registerCustomShader(customShader));
            }))
        .addStaticFunction("isShaderReady",
            luabridge::overload<const std::string&>(+[](const std::string& spec) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key) && ShaderPool::getShaderState(key) == ResourceLoadState::Finished;
            }),
            luabridge::overload<ShaderType, lua_Integer>(+[](ShaderType type, lua_Integer properties) {
                return ShaderPool::isShaderReady(type, static_cast<uint64_t>(properties));
            }))
        .addStaticFunction("isShaderFailed", +[](const std::string& spec) {
            ShaderKey key;
            return !ShaderPool::parseShaderSpec(spec, key) || ShaderPool::getShaderState(key) == ResourceLoadState::Failed;
        })
        .addStaticFunction("warmUpAllShaders", &ShaderPool::warmUpAllShaders)
        .addStaticFunction("getPendingShaderCount", &ShaderPool::getPendingShaderCount)
        .addStaticFunction("getPendingPipelineCount", &ShaderPool::getPendingPipelineCount)
        .addStaticFunction("isGpuCreationIdle", &ShaderPool::isGpuCreationIdle)
        .addStaticFunction("isRuntimeCompilationSupported", &ShaderPool::isRuntimeCompilationSupported)
        .addStaticProperty("runtimeCompilation", &ShaderPool::isRuntimeCompilationEnabled, &ShaderPool::setRuntimeCompilationEnabled)
        .addStaticProperty("asyncCompilation", &ShaderPool::isAsyncCompilation, &ShaderPool::setAsyncCompilation)
        .endClass();

    // the shader warm-up list a build ships (spec S32): progress of its replay at start
    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderWarmup>("ShaderWarmup")
        .addStaticFunction("isDone", &ShaderWarmup::isDone)
        .addStaticFunction("isActive", &ShaderWarmup::isActive)
        .addStaticFunction("getProgress", &ShaderWarmup::getProgress)
        .addStaticFunction("getEntryCount", &ShaderWarmup::getEntryCount)
        .addStaticFunction("getReadyCount", &ShaderWarmup::getReadyCount)
        .addStaticFunction("warmUp", +[]() { ShaderWarmup::begin(); })
        .endClass();

    // global shader values (Unity's Shader.SetGlobal*): read by every shader declaring them in
    // its u_vs_globals / u_fs_globals block (texture globals: a texture uniform of the name),
    // Project Settings > Graphics > Shader Globals declares them with their defaults
    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderGlobals>("Shader")
        .addStaticFunction("propertyToID", &luaShaderPropertyToID)
        .addStaticFunction("setGlobalFloat", &luaShaderSetGlobalFloat)
        .addStaticFunction("setGlobalInt", &luaShaderSetGlobalInt)
        .addStaticFunction("setGlobalVector", &luaShaderSetGlobalVector)
        .addStaticFunction("setGlobalColor", &luaShaderSetGlobalColor)
        .addStaticFunction("setGlobalMatrix", &luaShaderSetGlobalMatrix)
        .addStaticFunction("setGlobalFloatArray", &luaShaderSetGlobalFloatArray)
        .addStaticFunction("setGlobalVectorArray", &luaShaderSetGlobalVectorArray)
        .addStaticFunction("setGlobalTexture", &luaShaderSetGlobalTexture)
        .addStaticFunction("getGlobalFloat", &luaShaderGetGlobalFloat)
        .addStaticFunction("getGlobalInt", &luaShaderGetGlobalInt)
        .addStaticFunction("getGlobalVector", &luaShaderGetGlobalVector)
        .addStaticFunction("getGlobalColor", &luaShaderGetGlobalColor)
        .addStaticFunction("getGlobalMatrix", &luaShaderGetGlobalMatrix)
        .addStaticFunction("getGlobalFloatArray", &luaShaderGetGlobalFloatArray)
        .addStaticFunction("getGlobalVectorArray", &luaShaderGetGlobalVectorArray)
        .addStaticFunction("getGlobalTexture", &luaShaderGetGlobalTexture)
        .addStaticFunction("hasGlobal", &luaShaderHasGlobal)
        .addStaticFunction("getGlobalNames", &luaShaderGetGlobalNames)
        // back to the Project Settings defaults; the globals scripts made are dropped
        .addStaticFunction("resetGlobals", +[]() { ShaderGlobals::resetToDefaults(); })
        .endClass();

    // diagnostics: the pipelines renderable objects made and did not destroy yet
    luabridge::getGlobalNamespace(L)
        .beginClass<ObjectRender>("ObjectRender")
        .addStaticFunction("getLivePipelineCount", &ObjectRender::getLivePipelineCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderVariantCollection>("ShaderVariantCollection")
        .addConstructor<void (*) (void)>()
        .addFunction("add",
            luabridge::overload<ShaderVariantCollection*, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec) {
                return self->add(spec);
            }),
            luabridge::overload<ShaderVariantCollection*, const std::string&, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->add(spec, customShader);
            }),
            luabridge::overload<ShaderVariantCollection*, ShaderType, lua_Integer>(+[](ShaderVariantCollection* self, ShaderType type, lua_Integer properties) {
                return self->add(type, static_cast<uint64_t>(properties));
            }),
            luabridge::overload<ShaderVariantCollection*, ShaderType, lua_Integer, const std::string&>(+[](ShaderVariantCollection* self, ShaderType type, lua_Integer properties, const std::string& customShader) {
                return self->add(type, static_cast<uint64_t>(properties), customShader);
            }))
        .addFunction("addObject", +[](ShaderVariantCollection* self, EntityHandle* object) {
            return object ? self->addEntityVariants(object->getScene(), object->getEntity()) : (size_t)0;
        })
        .addFunction("addEntityVariants", &ShaderVariantCollection::addEntityVariants)
        .addFunction("remove",
            luabridge::overload<ShaderVariantCollection*, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec) {
                return self->remove(spec);
            }),
            luabridge::overload<ShaderVariantCollection*, const std::string&, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->remove(spec, customShader);
            }))
        .addFunction("contains",
            luabridge::overload<const ShaderVariantCollection*, const std::string&>(+[](const ShaderVariantCollection* self, const std::string& spec) {
                return self->contains(spec);
            }),
            luabridge::overload<const ShaderVariantCollection*, const std::string&, const std::string&>(+[](const ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->contains(spec, customShader);
            }))
        .addFunction("clear", &ShaderVariantCollection::clear)
        .addProperty("variantCount", &ShaderVariantCollection::getVariantCount)
        .addFunction("warmUp", &ShaderVariantCollection::warmUp)
        .addFunction("warmUpProgressively", &ShaderVariantCollection::warmUpProgressively)
        .addProperty("isWarmedUp", &ShaderVariantCollection::isWarmedUp)
        .addProperty("warmedUpVariantCount", &ShaderVariantCollection::getWarmedUpVariantCount)
        .addProperty("failedVariantCount", &ShaderVariantCollection::getFailedVariantCount)
        .endClass();

    // The "no entity" sentinel, mirroring the C++ NULL_ENTITY macro. Scene:findEntity
    // returns this when no match is found.
    luabridge::getGlobalNamespace(L)
        .addVariable("NULL_ENTITY", static_cast<Entity>(NULL_ENTITY));

    luabridge::getGlobalNamespace(L)
        .beginClass<Engine>("Engine")

        .addStaticProperty("scene", &Engine::getScene, &Engine::setScene)
        .addStaticFunction("setScene", &Engine::setScene)
        .addStaticFunction("getScene", &Engine::getScene)
        .addStaticFunction("addSceneLayer", &Engine::addSceneLayer)
        .addStaticFunction("executeSceneOnce", &Engine::executeSceneOnce)
        .addStaticFunction("removeScene", &Engine::removeScene)
        .addStaticFunction("removeAllSceneLayers", &Engine::removeAllSceneLayers)
        .addStaticFunction("removeAllScenes", &Engine::removeAllScenes)
        .addStaticFunction("isSceneRunning", &Engine::isSceneRunning)
        .addStaticFunction("hasScenesToExecuteOnce", &Engine::hasScenesToExecuteOnce)
        .addStaticFunction("getMainScene", &Engine::getMainScene)
        .addStaticFunction("getLastScene", &Engine::getLastScene)
        .addStaticFunction("pauseGameEvents", &Engine::pauseGameEvents)
        .addStaticProperty("canvasWidth", &Engine::getCanvasWidth)
        .addStaticProperty("canvasHeight", &Engine::getCanvasHeight)
        .addStaticFunction("setCanvasSize", &Engine::setCanvasSize)
        .addStaticProperty("preferredCanvasWidth", &Engine::getPreferredCanvasWidth)
        .addStaticProperty("preferredCanvasHeight", &Engine::getPreferredCanvasHeight)
        .addStaticProperty("viewRect", &Engine::getViewRect)
        .addStaticProperty("scalingMode", &Engine::getScalingMode, &Engine::setScalingMode)
        .addStaticFunction("getPixelsPerUnit", &Engine::getPixelsPerUnit)
        .addStaticFunction("setDefaultTextureFilter", &Engine::setDefaultTextureFilter)
        .addStaticFunction("getDefaultTextureFilter", &Engine::getDefaultTextureFilter)
        .addStaticProperty("textureStrategy", &Engine::getTextureStrategy, &Engine::setTextureStrategy)
        .addStaticFunction("setTextureQuality", &Engine::setTextureQuality)
        .addStaticFunction("getTextureQuality", &Engine::getTextureQuality)
        .addStaticFunction("setTextureMemoryBudget", &Engine::setTextureMemoryBudget)
        .addStaticFunction("getTextureMemoryBudget", &Engine::getTextureMemoryBudget)
        .addStaticFunction("getTextureMemoryUsage", [](){ return (double)Engine::getTextureMemoryUsage(); })
        .addStaticFunction("isTextureFormatSupported", &Engine::isTextureFormatSupported)
        .addStaticFunction("setTextureLowMemoryFallback", &Engine::setTextureLowMemoryFallback)
        .addStaticFunction("isTextureLowMemoryFallback", &Engine::isTextureLowMemoryFallback)
        .addStaticFunction("setAtmosphereQuality", &Engine::setAtmosphereQuality)
        .addStaticFunction("getAtmosphereQuality", &Engine::getAtmosphereQuality)
        .addStaticFunction("resetAtmosphereQuality", &Engine::resetAtmosphereQuality)
        .addStaticProperty("foliageDistanceScale", &Engine::getFoliageDistanceScale, &Engine::setFoliageDistanceScale)
        .addStaticFunction("resetFoliageDistanceScale", &Engine::resetFoliageDistanceScale)
        .addStaticProperty("callMouseInTouchEvent", &Engine::isCallMouseInTouchEvent, &Engine::setCallMouseInTouchEvent)
        .addStaticProperty("callTouchInMouseEvent", &Engine::isCallTouchInMouseEvent, &Engine::setCallTouchInMouseEvent)
        .addStaticFunction("setCallTouchInMouseEvent", &Engine::setCallTouchInMouseEvent)
        .addStaticProperty("useDegrees", &Engine::isUseDegrees, &Engine::setUseDegrees)
        .addStaticProperty("allowEventsOutCanvas", &Engine::isAllowEventsOutCanvas, &Engine::setAllowEventsOutCanvas)
        .addStaticProperty("ignoreEventsHandledByUI", &Engine::isIgnoreEventsHandledByUI, &Engine::setIgnoreEventsHandledByUI)
        .addStaticFunction("isUIEventReceived", &Engine::isUIEventReceived)
        .addStaticFunction("wantsGamepadNavigation", &Engine::wantsGamepadNavigation)
        .addStaticFunction("wantsKeyboardNavigation", &Engine::wantsKeyboardNavigation)
        .addStaticProperty("updateTime", &Engine::getUpdateTime, &Engine::setUpdateTime)
        .addStaticFunction("setUpdateTimeMS", &Engine::setUpdateTimeMS)
        .addStaticProperty("interpolationAlpha", &Engine::getInterpolationAlpha)
        .addStaticProperty("mouseCursor", &Engine::getMouseCursor, &Engine::setMouseCursor)
        .addStaticProperty("mouseMode", &Engine::getMouseMode, &Engine::setMouseMode)
        .addStaticFunction("setMousePosition", &Engine::setMousePosition)
        .addStaticProperty("platform", &Engine::getPlatform)
        .addStaticProperty("graphicBackend", &Engine::getGraphicBackend)
        .addStaticProperty("openGL", &Engine::isOpenGL)
        .addStaticProperty("framerate", &Engine::getFramerate)
        .addStaticProperty("deltatime", &Engine::getDeltatime)
        // the frame delta before timeScale, and its sum (Time.unscaledDeltaTime / unscaledTime)
        .addStaticProperty("unscaledDeltatime", &Engine::getUnscaledDeltatime)
        .addStaticProperty("unscaledTime", &Engine::getUnscaledTime)
        .addStaticProperty("maxDeltatime", &Engine::getMaxDeltatime, &Engine::setMaxDeltatime)
        .addStaticProperty("systemTime", &Engine::getSystemTime)
        .addStaticProperty("timeScale", &Engine::getTimeScale, &Engine::setTimeScale)
        .addStaticProperty("fixedDeltatime", &Engine::getFixedDeltatime, &Engine::setFixedDeltatime)
        .addStaticProperty("gameTime", &Engine::getGameTime)
        // a shipped game run with --headless, and the end of a run with its exit code (S30 R19)
        .addStaticFunction("isHeadless", &Engine::isHeadless)
        .addStaticFunction("quit", [] (std::optional<int> code) { Engine::quit(code.value_or(0)); })
        .addStaticProperty("frameCount", &Engine::getGameFrames)
        // Unity's Debug.isDebugBuild: the editor's Play and Debug-type exports
        .addStaticProperty("isDevelopmentBuild", &Engine::isDevelopmentBuild)
        .addStaticProperty("targetFrameRate", &Engine::getTargetFrameRate, &Engine::setTargetFrameRate)
        .addStaticProperty("vSyncCount", &Engine::getVSyncCount, &Engine::setVSyncCount)
        // Dynamic Batching (Project Settings > Graphics): the same image, fewer draw calls
        .addStaticProperty("dynamicBatching", &Engine::isDynamicBatching, &Engine::setDynamicBatching)
        .addStaticProperty("isFocused", &Engine::isFocused)
        // false while the window can't be seen: the game updates without drawing (desktops)
        .addStaticProperty("isVisible", &Engine::isVisible)
        .addStaticProperty("asyncLoading", &Engine::isAsyncLoading, &Engine::setAsyncLoading)
        .addStaticFunction("startAsyncThread", &Engine::startAsyncThread)
        .addStaticFunction("commitThreadQueue", &Engine::commitThreadQueue)
        .addStaticFunction("endAsyncThread", &Engine::endAsyncThread)
        .addStaticFunction("isAsyncThread", &Engine::isAsyncThread)
        .addStaticFunction("isViewLoaded", &Engine::isViewLoaded)
        .addStaticFunction("setMaxResourceLoadingThreads", &Engine::setMaxResourceLoadingThreads)
        .addStaticFunction("getQueuedResourceCount", &Engine::getQueuedResourceCount)
        .addStaticProperty("framebuffer", &Engine::getFramebuffer, &Engine::setFramebuffer)
        .addStaticFunction("clearAllSubscriptions", &Engine::clearAllSubscriptions)
        .addStaticFunction("clearComponentSubscriptions", &Engine::clearComponentSubscriptions)

        .addStaticProperty("onViewLoaded", [] () { return &Engine::onViewLoaded; }, [] (lua_State* L) { Engine::onViewLoaded = L; })
        .addStaticProperty("onCanvasChanged", [] () { return &Engine::onViewChanged; }, [] (lua_State* L) { Engine::onViewChanged = L; })
        .addStaticProperty("onViewDestroyed", [] () { return &Engine::onViewDestroyed; }, [] (lua_State* L) { Engine::onViewDestroyed = L; })
        .addStaticProperty("onDraw", [] () { return &Engine::onDraw; }, [] (lua_State* L) { Engine::onDraw = L; })
        .addStaticProperty("onUpdate", [] () { return &Engine::onUpdate; }, [] (lua_State* L) { Engine::onUpdate = L; })
        .addStaticProperty("onFixedUpdate", [] () { return &Engine::onFixedUpdate; }, [] (lua_State* L) { Engine::onFixedUpdate = L; })
        .addStaticProperty("onPostUpdate", [] () { return &Engine::onPostUpdate; }, [] (lua_State* L) { Engine::onPostUpdate = L; })
        .addStaticProperty("onPause", [] () { return &Engine::onPause; }, [] (lua_State* L) { Engine::onPause = L; })
        .addStaticProperty("onResume", [] () { return &Engine::onResume; }, [] (lua_State* L) { Engine::onResume = L; })
        .addStaticProperty("onShutdown", [] () { return &Engine::onShutdown; }, [] (lua_State* L) { Engine::onShutdown = L; })
        .addStaticProperty("onFocus", [] () { return &Engine::onFocus; }, [] (lua_State* L) { Engine::onFocus = L; })
        .addStaticProperty("onVisibilityChanged", [] () { return &Engine::onVisibilityChanged; }, [] (lua_State* L) { Engine::onVisibilityChanged = L; })
        .addStaticProperty("onOrientationChanged", [] () { return &Engine::onOrientationChanged; }, [] (lua_State* L) { Engine::onOrientationChanged = L; })
        .addStaticProperty("onLowMemory", [] () { return &Engine::onLowMemory; }, [] (lua_State* L) { Engine::onLowMemory = L; })
        .addStaticProperty("onSafeAreaChanged", [] () { return &Engine::onSafeAreaChanged; }, [] (lua_State* L) { Engine::onSafeAreaChanged = L; })
        .addStaticProperty("onSceneLoaded", [] () { return &Engine::onSceneLoaded; }, [] (lua_State* L) { Engine::onSceneLoaded = L; })
        .addStaticProperty("onTouchStart", [] () { return &Engine::onTouchStart; }, [] (lua_State* L) { Engine::onTouchStart = L; })
        .addStaticProperty("onTouchEnd", [] () { return &Engine::onTouchEnd; }, [] (lua_State* L) { Engine::onTouchEnd = L; })
        .addStaticProperty("onTouchMove", [] () { return &Engine::onTouchMove; }, [] (lua_State* L) { Engine::onTouchMove = L; })
        .addStaticProperty("onTouchCancel", [] () { return &Engine::onTouchCancel; }, [] (lua_State* L) { Engine::onTouchCancel = L; })
        .addStaticProperty("onMouseDown", [] () { return &Engine::onMouseDown; }, [] (lua_State* L) { Engine::onMouseDown = L; })
        .addStaticProperty("onMouseUp", [] () { return &Engine::onMouseUp; }, [] (lua_State* L) { Engine::onMouseUp = L; })
        .addStaticProperty("onMouseScroll", [] () { return &Engine::onMouseScroll; }, [] (lua_State* L) { Engine::onMouseScroll = L; })
        .addStaticProperty("onMouseMove", [] () { return &Engine::onMouseMove; }, [] (lua_State* L) { Engine::onMouseMove = L; })
        .addStaticProperty("onMouseEnter", [] () { return &Engine::onMouseEnter; }, [] (lua_State* L) { Engine::onMouseEnter = L; })
        .addStaticProperty("onMouseLeave", [] () { return &Engine::onMouseLeave; }, [] (lua_State* L) { Engine::onMouseLeave = L; })
        .addStaticProperty("onKeyDown", [] () { return &Engine::onKeyDown; }, [] (lua_State* L) { Engine::onKeyDown = L; })
        .addStaticProperty("onKeyUp", [] () { return &Engine::onKeyUp; }, [] (lua_State* L) { Engine::onKeyUp = L; })
        .addStaticProperty("onCharInput", [] () { return &Engine::onCharInput; }, [] (lua_State* L) { Engine::onCharInput = L; })
        .addStaticProperty("onGamepadConnect", [] () { return &Engine::onGamepadConnect; }, [] (lua_State* L) { Engine::onGamepadConnect = L; })
        .addStaticProperty("onGamepadDisconnect", [] () { return &Engine::onGamepadDisconnect; }, [] (lua_State* L) { Engine::onGamepadDisconnect = L; })
        .addStaticProperty("onGamepadButtonDown", [] () { return &Engine::onGamepadButtonDown; }, [] (lua_State* L) { Engine::onGamepadButtonDown = L; })
        .addStaticProperty("onGamepadButtonUp", [] () { return &Engine::onGamepadButtonUp; }, [] (lua_State* L) { Engine::onGamepadButtonUp = L; })
        .addStaticProperty("onGamepadAxisMove", [] () { return &Engine::onGamepadAxisMove; }, [] (lua_State* L) { Engine::onGamepadAxisMove = L; })
        // an input action's button went down / up this frame (its Input Manager name: "Jump", else
        // "Map/Action"), before onUpdate
        .addStaticProperty("onButtonDown", [] () { return &Engine::onButtonDown; }, [] (lua_State* L) { Engine::onButtonDown = L; })
        .addStaticProperty("onButtonUp", [] () { return &Engine::onButtonUp; }, [] (lua_State* L) { Engine::onButtonUp = L; })

        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PostProcessPass>("PostProcessPass")
        .addConstructor<void (*) (void)>()
        .addProperty("shader", &PostProcessPass::shader, &PostProcessPass::shader)
        .addProperty("enabled", &PostProcessPass::enabled, &PostProcessPass::enabled)
        .addFunction("setUniform",
            luabridge::overload<const std::string&, const Vector4&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, const Vector3&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, const Vector2&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, float>(&PostProcessPass::setUniform))
        .addFunction("getUniform", &PostProcessPass::getUniform)
        .addFunction("removeUniform", &PostProcessPass::removeUniform)
        // (phase 4) the pass's own textures by uniform, and its resolution scale
        .addFunction("setTexture", &PostProcessPass::setTexture)
        .addFunction("getTexture", &PostProcessPass::getTexture)
        .addProperty("scale", &PostProcessPass::scale, &PostProcessPass::scale)
        .endClass();

    // a copy: edit it, then assign it back (scene.postProcessing = post)
    luabridge::getGlobalNamespace(L)
        .beginClass<PostProcessingSettings>("PostProcessingSettings")
        .addConstructor<void (*) (void)>()
        .addProperty("exposure", &PostProcessingSettings::exposure, &PostProcessingSettings::exposure)
        .addProperty("tonemap", &PostProcessingSettings::tonemap, &PostProcessingSettings::tonemap)
        .addProperty("colorGradingEnabled", &PostProcessingSettings::colorGradingEnabled, &PostProcessingSettings::colorGradingEnabled)
        .addProperty("temperature", &PostProcessingSettings::temperature, &PostProcessingSettings::temperature)
        .addProperty("tint", &PostProcessingSettings::tint, &PostProcessingSettings::tint)
        .addProperty("contrast", &PostProcessingSettings::contrast, &PostProcessingSettings::contrast)
        .addProperty("saturation", &PostProcessingSettings::saturation, &PostProcessingSettings::saturation)
        .addProperty("lift", &PostProcessingSettings::lift, &PostProcessingSettings::lift)
        .addProperty("gamma", &PostProcessingSettings::gamma, &PostProcessingSettings::gamma)
        .addProperty("gain", &PostProcessingSettings::gain, &PostProcessingSettings::gain)
        .addProperty("lutTexture", &PostProcessingSettings::lutTexture, &PostProcessingSettings::lutTexture)
        .addProperty("lutContribution", &PostProcessingSettings::lutContribution, &PostProcessingSettings::lutContribution)
        .addProperty("lutInputEncoding", &PostProcessingSettings::lutInputEncoding, &PostProcessingSettings::lutInputEncoding)
        .addProperty("lutSize", &PostProcessingSettings::lutSize, &PostProcessingSettings::lutSize)
        .addProperty("monochromeEnabled", &PostProcessingSettings::monochromeEnabled, &PostProcessingSettings::monochromeEnabled)
        .addProperty("monochromeMode", &PostProcessingSettings::monochromeMode, &PostProcessingSettings::monochromeMode)
        .addProperty("monochromeIntensity", &PostProcessingSettings::monochromeIntensity, &PostProcessingSettings::monochromeIntensity)
        .addProperty("vignetteEnabled", &PostProcessingSettings::vignetteEnabled, &PostProcessingSettings::vignetteEnabled)
        .addProperty("vignetteIntensity", &PostProcessingSettings::vignetteIntensity, &PostProcessingSettings::vignetteIntensity)
        .addProperty("vignetteSmoothness", &PostProcessingSettings::vignetteSmoothness, &PostProcessingSettings::vignetteSmoothness)
        .addProperty("vignetteRoundness", &PostProcessingSettings::vignetteRoundness, &PostProcessingSettings::vignetteRoundness)
        .addProperty("vignetteColor", &PostProcessingSettings::vignetteColor, &PostProcessingSettings::vignetteColor)
        .addProperty("ditherEnabled", &PostProcessingSettings::ditherEnabled, &PostProcessingSettings::ditherEnabled)
        .addProperty("fxaaEnabled", &PostProcessingSettings::fxaaEnabled, &PostProcessingSettings::fxaaEnabled)
        .addProperty("chromaticAberrationEnabled", &PostProcessingSettings::chromaticAberrationEnabled, &PostProcessingSettings::chromaticAberrationEnabled)
        .addProperty("chromaticAberrationIntensity", &PostProcessingSettings::chromaticAberrationIntensity, &PostProcessingSettings::chromaticAberrationIntensity)
        .addProperty("pixelateEnabled", &PostProcessingSettings::pixelateEnabled, &PostProcessingSettings::pixelateEnabled)
        .addProperty("pixelateSize", &PostProcessingSettings::pixelateSize, &PostProcessingSettings::pixelateSize)
        .addProperty("crtEnabled", &PostProcessingSettings::crtEnabled, &PostProcessingSettings::crtEnabled)
        .addProperty("crtScanlines", &PostProcessingSettings::crtScanlines, &PostProcessingSettings::crtScanlines)
        .addProperty("crtMask", &PostProcessingSettings::crtMask, &PostProcessingSettings::crtMask)
        .addProperty("crtCurvature", &PostProcessingSettings::crtCurvature, &PostProcessingSettings::crtCurvature)
        .addProperty("crtVignette", &PostProcessingSettings::crtVignette, &PostProcessingSettings::crtVignette)
        .addProperty("edgeDetectEnabled", &PostProcessingSettings::edgeDetectEnabled, &PostProcessingSettings::edgeDetectEnabled)
        .addProperty("edgeDetectThreshold", &PostProcessingSettings::edgeDetectThreshold, &PostProcessingSettings::edgeDetectThreshold)
        .addProperty("edgeDetectIntensity", &PostProcessingSettings::edgeDetectIntensity, &PostProcessingSettings::edgeDetectIntensity)
        .addProperty("edgeDetectColor", &PostProcessingSettings::edgeDetectColor, &PostProcessingSettings::edgeDetectColor)
        .addProperty("filmGrainEnabled", &PostProcessingSettings::filmGrainEnabled, &PostProcessingSettings::filmGrainEnabled)
        .addProperty("filmGrainIntensity", &PostProcessingSettings::filmGrainIntensity, &PostProcessingSettings::filmGrainIntensity)
        .addProperty("filmGrainResponse", &PostProcessingSettings::filmGrainResponse, &PostProcessingSettings::filmGrainResponse)
        .addProperty("filmGrainType", &PostProcessingSettings::filmGrainType, &PostProcessingSettings::filmGrainType)
        .addProperty("filmGrainSize", &PostProcessingSettings::filmGrainSize, &PostProcessingSettings::filmGrainSize)
        .addProperty("filmGrainColored", &PostProcessingSettings::filmGrainColored, &PostProcessingSettings::filmGrainColored)
        .addProperty("filmFrameRate", &PostProcessingSettings::filmFrameRate, &PostProcessingSettings::filmFrameRate)
        .addProperty("vhsEnabled", &PostProcessingSettings::vhsEnabled, &PostProcessingSettings::vhsEnabled)
        .addProperty("vhsIntensity", &PostProcessingSettings::vhsIntensity, &PostProcessingSettings::vhsIntensity)
        .addProperty("vhsWobble", &PostProcessingSettings::vhsWobble, &PostProcessingSettings::vhsWobble)
        .addProperty("vhsTracking", &PostProcessingSettings::vhsTracking, &PostProcessingSettings::vhsTracking)
        .addProperty("vhsColorBleed", &PostProcessingSettings::vhsColorBleed, &PostProcessingSettings::vhsColorBleed)
        .addProperty("vhsTapeNoise", &PostProcessingSettings::vhsTapeNoise, &PostProcessingSettings::vhsTapeNoise)
        .addProperty("vhsChromaStreaks", &PostProcessingSettings::vhsChromaStreaks, &PostProcessingSettings::vhsChromaStreaks)
        .addProperty("vhsDropouts", &PostProcessingSettings::vhsDropouts, &PostProcessingSettings::vhsDropouts)
        .addProperty("vhsScanlines", &PostProcessingSettings::vhsScanlines, &PostProcessingSettings::vhsScanlines)
        .addProperty("vhsInterference", &PostProcessingSettings::vhsInterference, &PostProcessingSettings::vhsInterference)
        .addProperty("vhsGlitches", &PostProcessingSettings::vhsGlitches, &PostProcessingSettings::vhsGlitches)
        .addProperty("vhsSpeed", &PostProcessingSettings::vhsSpeed, &PostProcessingSettings::vhsSpeed)
        .addProperty("filmDamageEnabled", &PostProcessingSettings::filmDamageEnabled, &PostProcessingSettings::filmDamageEnabled)
        .addProperty("filmDamageIntensity", &PostProcessingSettings::filmDamageIntensity, &PostProcessingSettings::filmDamageIntensity)
        .addProperty("filmHairAmount", &PostProcessingSettings::filmHairAmount, &PostProcessingSettings::filmHairAmount)
        .addProperty("filmScratchAmount", &PostProcessingSettings::filmScratchAmount, &PostProcessingSettings::filmScratchAmount)
        .addProperty("filmDustAmount", &PostProcessingSettings::filmDustAmount, &PostProcessingSettings::filmDustAmount)
        .addProperty("filmDustSize", &PostProcessingSettings::filmDustSize, &PostProcessingSettings::filmDustSize)
        .addProperty("filmGateWeave", &PostProcessingSettings::filmGateWeave, &PostProcessingSettings::filmGateWeave)
        .addProperty("filmGateMask", &PostProcessingSettings::filmGateMask, &PostProcessingSettings::filmGateMask)
        .addProperty("filmGateMaskFeather", &PostProcessingSettings::filmGateMaskFeather, &PostProcessingSettings::filmGateMaskFeather)
        .addProperty("sharpenEnabled", &PostProcessingSettings::sharpenEnabled, &PostProcessingSettings::sharpenEnabled)
        .addProperty("sharpenIntensity", &PostProcessingSettings::sharpenIntensity, &PostProcessingSettings::sharpenIntensity)
        .addProperty("bloomEnabled", &PostProcessingSettings::bloomEnabled, &PostProcessingSettings::bloomEnabled)
        .addProperty("bloomThreshold", &PostProcessingSettings::bloomThreshold, &PostProcessingSettings::bloomThreshold)
        .addProperty("bloomKnee", &PostProcessingSettings::bloomKnee, &PostProcessingSettings::bloomKnee)
        .addProperty("bloomIntensity", &PostProcessingSettings::bloomIntensity, &PostProcessingSettings::bloomIntensity)
        .addProperty("bloomTint", &PostProcessingSettings::bloomTint, &PostProcessingSettings::bloomTint)
        .addProperty("bloomScatter", &PostProcessingSettings::bloomScatter, &PostProcessingSettings::bloomScatter)
        .addProperty("blurEnabled", &PostProcessingSettings::blurEnabled, &PostProcessingSettings::blurEnabled)
        .addProperty("blurRadius", &PostProcessingSettings::blurRadius, &PostProcessingSettings::blurRadius)
        .addProperty("blurIntensity", &PostProcessingSettings::blurIntensity, &PostProcessingSettings::blurIntensity)
        .addProperty("halationEnabled", &PostProcessingSettings::halationEnabled, &PostProcessingSettings::halationEnabled)
        .addProperty("halationIntensity", &PostProcessingSettings::halationIntensity, &PostProcessingSettings::halationIntensity)
        .addProperty("halationThreshold", &PostProcessingSettings::halationThreshold, &PostProcessingSettings::halationThreshold)
        .addProperty("halationRadius", &PostProcessingSettings::halationRadius, &PostProcessingSettings::halationRadius)
        .addProperty("halationTint", &PostProcessingSettings::halationTint, &PostProcessingSettings::halationTint)
        .addProperty("renderScale", &PostProcessingSettings::renderScale, &PostProcessingSettings::renderScale)
        .addProperty("msaaSamples", &PostProcessingSettings::msaaSamples, &PostProcessingSettings::msaaSamples)
        .addProperty("dynamicResolutionEnabled", &PostProcessingSettings::dynamicResolutionEnabled, &PostProcessingSettings::dynamicResolutionEnabled)
        .addProperty("dynamicResolutionMinScale", &PostProcessingSettings::dynamicResolutionMinScale, &PostProcessingSettings::dynamicResolutionMinScale)
        .addProperty("dynamicResolutionTargetFps", &PostProcessingSettings::dynamicResolutionTargetFps, &PostProcessingSettings::dynamicResolutionTargetFps)
        // Palette Quantize (S43)
        .addProperty("paletteEnabled", &PostProcessingSettings::paletteEnabled, &PostProcessingSettings::paletteEnabled)
        .addProperty("palette", &PostProcessingSettings::palette, &PostProcessingSettings::palette)
        .addProperty("paletteDither", &PostProcessingSettings::paletteDither, &PostProcessingSettings::paletteDither)
        .addProperty("paletteDitherStrength", &PostProcessingSettings::paletteDitherStrength, &PostProcessingSettings::paletteDitherStrength)
        .addProperty("paletteDistance", &PostProcessingSettings::paletteDistance, &PostProcessingSettings::paletteDistance)
        .addProperty("palettePrecision", &PostProcessingSettings::palettePrecision, &PostProcessingSettings::palettePrecision)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<SceneManager>("SceneManager")
        .addStaticFunction("registerScene",
            luabridge::overload<uint32_t, const std::string&, std::function<void()>>(&SceneManager::registerScene),
            luabridge::overload<uint32_t, const std::string&, std::function<void()>, std::function<void()>>(&SceneManager::registerScene))
        .addStaticFunction("loadScene", 
            luabridge::overload<uint32_t>(&SceneManager::loadScene),
            luabridge::overload<const std::string&>(&SceneManager::loadScene))
        .addStaticFunction("addChildScene",
            luabridge::overload<uint32_t>(&SceneManager::addChildScene),
            luabridge::overload<const std::string&>(&SceneManager::addChildScene))
        .addStaticFunction("removeChildScene",
            luabridge::overload<uint32_t>(&SceneManager::removeChildScene),
            luabridge::overload<const std::string&>(&SceneManager::removeChildScene))
        .addStaticFunction("getSceneId", &SceneManager::getSceneId)
        .addStaticFunction("getSceneName", &SceneManager::getSceneName)
        .addStaticFunction("getSceneNames", &SceneManager::getSceneNames)
        .addStaticProperty("sceneCount", &SceneManager::getSceneCount)
        .addStaticProperty("currentSceneId", &SceneManager::getCurrentSceneId)
        .addStaticProperty("currentSceneName", &SceneManager::getCurrentSceneName)
        .addStaticProperty("loadPending", &SceneManager::isLoadPending)
        .addStaticFunction("setLoadingScene",
            luabridge::overload<uint32_t>(&SceneManager::setLoadingScene),
            luabridge::overload<const std::string&>(&SceneManager::setLoadingScene))
        .addStaticProperty("loadingSceneId", &SceneManager::getLoadingSceneId)
        .addStaticProperty("loadingDelay", &SceneManager::getLoadingDelay, &SceneManager::setLoadingDelay)
        .addStaticProperty("loadingTimeout", &SceneManager::getLoadingTimeout, &SceneManager::setLoadingTimeout)
        .addStaticProperty("loading", &SceneManager::isLoading)
        .addStaticProperty("loadingProgress", &SceneManager::getLoadingProgress)
        .addStaticFunction("holdLoading", &SceneManager::holdLoading)
        .addStaticFunction("releaseLoading", &SceneManager::releaseLoading)
        .addStaticFunction("isCoveredByLoading", &SceneManager::isCoveredByLoading)
        .addStaticFunction("preloadScene",
            luabridge::overload<uint32_t>(&SceneManager::preloadScene),
            luabridge::overload<const std::string&>(&SceneManager::preloadScene))
        .addStaticFunction("getPreloadProgress",
            luabridge::overload<uint32_t>(&SceneManager::getPreloadProgress),
            luabridge::overload<const std::string&>(&SceneManager::getPreloadProgress))
        .addStaticFunction("cancelPreload",
            luabridge::overload<uint32_t>(&SceneManager::cancelPreload),
            luabridge::overload<const std::string&>(&SceneManager::cancelPreload))
        // the DontDestroyOnLoad scene, nil before the first DontDestroyOnLoad
        .addStaticProperty("persistentScene", &SceneManager::getPersistentScene)
        .addStaticFunction("moveGameObjectToScene", &LuaBinding::luaMoveGameObjectToScene)
        .addStaticFunction("clearAll", &SceneManager::clearAll)
        .addStaticFunction("setScenePtr", &SceneManager::setScenePtr)
        .addStaticFunction("getScenePtr", &SceneManager::getScenePtr)
        .addStaticFunction("removeScenePtr", &SceneManager::removeScenePtr)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PrefabManager>("PrefabManager")
        .addStaticFunction("findPrefab", &PrefabManager::findPrefab)
        .addStaticFunction("getPrefabId", &PrefabManager::getPrefabId)
        .addStaticFunction("getPrefabName", &PrefabManager::getPrefabName)
        .addStaticFunction("getPrefabGuid", &PrefabManager::getPrefabGuid)
        .addStaticFunction("getPrefabNames", &PrefabManager::getPrefabNames)
        .addStaticProperty("prefabCount", &PrefabManager::getPrefabCount)
        .endClass();

    // What a Prefab script property holds; GameObject.instantiate takes it
    luabridge::getGlobalNamespace(L)
        .beginClass<PrefabAsset>("PrefabAsset")
        .addConstructor<void (*) (), void (*) (const std::string&, const std::string&)>()
        // Lua reads a number as a string too, so the id overload comes first
        .addStaticFunction("load",
            luabridge::overload<uint32_t>(&PrefabAsset::load),
            luabridge::overload<const std::string&>(&PrefabAsset::load))
        .addProperty("id", &PrefabAsset::getId)
        .addProperty("valid", &PrefabAsset::isValid)
        .addProperty("guid", [](const PrefabAsset* self) { return self->getGuid(); })
        .addProperty("path", [](const PrefabAsset* self) { return self->getPath(); })
        .addProperty("name", &PrefabAsset::getName)
        .addFunction("isValid", &PrefabAsset::isValid)
        .addFunction("__eq", &PrefabAsset::operator==)
        .addFunction("__tostring", [](const PrefabAsset* self) { return "PrefabAsset(" + self->getPath() + ")"; })
        .endClass();

    // Asset identity (S04): what an Asset script property holds, and Asset.ref returns
    luabridge::getGlobalNamespace(L)
        .beginClass<ScriptAssetRef>("AssetRef")
        .addProperty("guid", &ScriptAssetRef::getGuid)
        .addProperty("path", &ScriptAssetRef::getPath)
        .addProperty("type", &ScriptAssetRef::getType)
        .addProperty("valid", &ScriptAssetRef::isValid)
        .addFunction("isValid", &ScriptAssetRef::isValid)
        .addFunction("__eq", &ScriptAssetRef::operator==)
        .addFunction("__tostring", [](const ScriptAssetRef* self) { return "AssetRef(" + self->getPath() + ")"; })
        .endClass();

    // Asset.path(guid) -> string|nil, Asset.guid(path) -> string|nil,
    // Asset.exists(guidOrPath) -> bool, Asset.ref(guidOrPath) -> AssetRef
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Asset")
        .addFunction("path", +[](lua_State* state) -> int {
            const char* text = luaL_checkstring(state, 1);
            std::string path;
            if (Guid::isValidText(text) && AssetMap::findPath(Guid::parse(text), path)) {
                lua_pushstring(state, path.c_str());
            } else {
                lua_pushnil(state);
            }
            return 1;
        })
        .addFunction("guid", +[](lua_State* state) -> int {
            const Guid guid = AssetMap::findGuid(luaL_checkstring(state, 1));
            if (guid.isNull()) {
                lua_pushnil(state);
            } else {
                lua_pushstring(state, guid.str().c_str());
            }
            return 1;
        })
        .addFunction("exists", +[](lua_State* state) -> int {
            lua_pushboolean(state, ScriptAssetRef::make(luaL_checkstring(state, 1)).isValid());
            return 1;
        })
        .addFunction("ref", +[](lua_State* state) -> int {
            if (!luabridge::push<ScriptAssetRef>(state, ScriptAssetRef::make(luaL_checkstring(state, 1)))) {
                lua_pushnil(state);
            }
            return 1;
        })
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void()>>("FunctionSubscribe_V")
        .addFunction("__call", &FunctionSubscribe<void()>::call)
        .addFunction("call", &FunctionSubscribe<void()>::call)
        .addFunction("add", (bool (FunctionSubscribe<void()>::*)(const std::string&, lua_State*))&FunctionSubscribe<void()>::add)
        .addFunction("remove", &FunctionSubscribe<void()>::remove)
        .addFunction("has", &FunctionSubscribe<void()>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(bool)>>("FunctionSubscribe_V_B")
        .addFunction("__call", &FunctionSubscribe<void(bool)>::call)
        .addFunction("call", &FunctionSubscribe<void(bool)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(bool)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(bool)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float)>>("FunctionSubscribe_V_F")
        .addFunction("__call", &FunctionSubscribe<void(float)>::call)
        .addFunction("call", &FunctionSubscribe<void(float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float)>::add)
        .addFunction("remove", &FunctionSubscribe<void(float)>::remove)
        .addFunction("has", &FunctionSubscribe<void(float)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float,float)>>("FunctionSubscribe_V_FF")
        .addFunction("__call", &FunctionSubscribe<void(float,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(float,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float,float)>::add)
        .addFunction("remove", &FunctionSubscribe<void(float,float)>::remove)
        .addFunction("has", &FunctionSubscribe<void(float,float)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float,float,int)>>("FunctionSubscribe_V_FFI")
        .addFunction("__call", &FunctionSubscribe<void(float,float,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(float,float,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float,float,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float,float,int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(float,float,int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(float,float,int)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int)>>("FunctionSubscribe_V_I")
        .addFunction("__call", &FunctionSubscribe<void(int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,int)>>("FunctionSubscribe_V_II")
        .addFunction("__call", &FunctionSubscribe<void(int,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int,int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int,int)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,int,float)>>("FunctionSubscribe_V_IIF")
        .addFunction("__call", &FunctionSubscribe<void(int,int,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,int,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,int,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,int,float)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int,int,float)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int,int,float)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,float,float)>>("FunctionSubscribe_V_IFF")
        .addFunction("__call", &FunctionSubscribe<void(int,float,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,float,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,float,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,float,float)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int,float,float)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int,float,float)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,float,float,int)>>("FunctionSubscribe_V_IFFI")
        .addFunction("__call", &FunctionSubscribe<void(int,float,float,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,float,float,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,float,float,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,float,float,int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int,float,float,int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int,float,float,int)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,bool,int)>>("FunctionSubscribe_V_IBI")
        .addFunction("__call", &FunctionSubscribe<void(int,bool,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,bool,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,bool,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,bool,int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int,bool,int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int,bool,int)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(char32_t)>>("FunctionSubscribe_V_W")
        .addFunction("__call", &FunctionSubscribe<void(char32_t)>::call)
        .addFunction("call", &FunctionSubscribe<void(char32_t)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(char32_t)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(char32_t)>::add)
        .addFunction("remove", &FunctionSubscribe<void(char32_t)>::remove)
        .addFunction("has", &FunctionSubscribe<void(char32_t)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::string&)>>("FunctionSubscribe_V_S")
        .addFunction("__call", &FunctionSubscribe<void(const std::string&)>::call)
        .addFunction("call", &FunctionSubscribe<void(const std::string&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(const std::string&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::string&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::string&)>::remove)
        .addFunction("has", &FunctionSubscribe<void(const std::string&)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const AnimationEvent&)>>("FunctionSubscribe_V_AE")
        .addFunction("__call", &FunctionSubscribe<void(const AnimationEvent&)>::call)
        .addFunction("call", &FunctionSubscribe<void(const AnimationEvent&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(const AnimationEvent&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const AnimationEvent&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const AnimationEvent&)>::remove)
        .addFunction("has", &FunctionSubscribe<void(const AnimationEvent&)>::has)
        .endClass();

    // Vertex Animator instance events: (instance, event) and (instance, clip)
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int, const AnimationEvent&)>>("FunctionSubscribe_V_IAE")
        .addFunction("__call", &FunctionSubscribe<void(int, const AnimationEvent&)>::call)
        .addFunction("call", &FunctionSubscribe<void(int, const AnimationEvent&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int, const AnimationEvent&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int, const AnimationEvent&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int, const AnimationEvent&)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int, const AnimationEvent&)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int, const std::string&)>>("FunctionSubscribe_V_IS")
        .addFunction("__call", &FunctionSubscribe<void(int, const std::string&)>::call)
        .addFunction("call", &FunctionSubscribe<void(int, const std::string&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int, const std::string&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int, const std::string&)>::add)
        .addFunction("remove", &FunctionSubscribe<void(int, const std::string&)>::remove)
        .addFunction("has", &FunctionSubscribe<void(int, const std::string&)>::has)
        .endClass();

    // the Animator's callbacks (onStateEnter / onStateExit, onMontageEnded, onAnimatorMove)
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::string&, const std::string&)>>("FunctionSubscribe_V_SS")
        .addFunction("__call", &FunctionSubscribe<void(const std::string&, const std::string&)>::call)
        .addFunction("call", &FunctionSubscribe<void(const std::string&, const std::string&)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(const std::string&, const std::string&)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::string&, const std::string&)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::string&, bool)>>("FunctionSubscribe_V_SB")
        .addFunction("__call", &FunctionSubscribe<void(const std::string&, bool)>::call)
        .addFunction("call", &FunctionSubscribe<void(const std::string&, bool)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(const std::string&, bool)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::string&, bool)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Vector3, Quaternion)>>("FunctionSubscribe_V_V3Q")
        .addFunction("__call", &FunctionSubscribe<void(Vector3, Quaternion)>::call)
        .addFunction("call", &FunctionSubscribe<void(Vector3, Quaternion)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Vector3, Quaternion)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Vector3, Quaternion)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<float(float)>>("FunctionSubscribe_F_F")
        .addFunction("__call", &FunctionSubscribe<float(float)>::call)
        .addFunction("call", &FunctionSubscribe<float(float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<float(float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<float(float)>::add)
        .addFunction("remove", &FunctionSubscribe<float(float)>::remove)
        .addFunction("has", &FunctionSubscribe<float(float)>::has)
        .endClass();

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>>("FunctionSubscribe_V_B2ULB2UL")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::add)
        .addFunction("remove", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::remove)
        .addFunction("has", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>>("FunctionSubscribe_V_B2ULB2ULV2V2F")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::add)
        .addFunction("remove", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::remove)
        .addFunction("has", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>>("FunctionSubscribe_B_B2ULB2UL")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::add)
        .addFunction("remove", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::remove)
        .addFunction("has", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>>("FunctionSubscribe_B_B2ULB2ULM2")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::add)
        .addFunction("remove", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::remove)
        .addFunction("has", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::has)
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody)>>("FunctionSubscribe_V_B3")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody)>::add)
        .addFunction("remove", &FunctionSubscribe<void(Rigidbody)>::remove)
        .addFunction("has", &FunctionSubscribe<void(Rigidbody)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>>("FunctionSubscribe_V_B3B3C3")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::add)
        .addFunction("remove", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::remove)
        .addFunction("has", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>>("FunctionSubscribe_V_B3B3ULUL")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::add)
        .addFunction("remove", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::remove)
        .addFunction("has", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::has)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>>("FunctionSubscribe_B_B3B3V3CR3")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::add)
        .addFunction("remove", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::remove)
        .addFunction("has", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::has)
        .endClass();
#endif

    luabridge::getGlobalNamespace(L)
        .beginClass<Texture>("Texture")
        .addConstructor <void (*) (void), void (*) (const std::string&), void (*) (const std::string&, TextureData), void (*) (Framebuffer*)> ()
        .addFunction("setPath", &Texture::setPath)
        .addFunction("setData", &Texture::setData)
        .addFunction("setId", &Texture::setId)
        .addFunction("setCubePath", &Texture::setCubePath)
        .addFunction("setCubePaths", &Texture::setCubePaths)
        .addFunction("setFramebuffer", &Texture::setFramebuffer)
        .addFunction("load", &Texture::load)
        .addFunction("destroy", &Texture::destroy)
        .addFunction("getRender", &Texture::getRender)
        .addFunction("getPath", &Texture::getPath)
        .addFunction("getData", [](const Texture* texture) -> TextureData& {
            // cooked block formats have no pixels the CPU can read (spec S14 R3)
            if (texture->hasData() && texture->getData().isCompressed()){
                static std::set<std::string> logged;
                if (logged.insert(texture->getPath()).second)
                    Log::warn("%s: pixels are GPU-compressed; enable Read/Write in its import settings", texture->getPath().c_str());
                static TextureData empty;
                return empty;
            }
            return texture->getData();
        })
        .addFunction("isCompressed", &Texture::isCompressed)
        .addFunction("getGpuFormat", &Texture::getGpuFormat)
        .addFunction("getStoredWidth", &Texture::getStoredWidth)
        .addFunction("getStoredHeight", &Texture::getStoredHeight)
        .addFunction("getId", &Texture::getId)
        .addFunction("getNumFaces", &Texture::getNumFaces)
        .addFunction("getType", &Texture::getType)
        .addFunction("isCubeMap", &Texture::isCubeMap)
        .addFunction("getWidth", &Texture::getWidth)
        .addFunction("getHeight", &Texture::getHeight)
        .addFunction("isTransparent", &Texture::isTransparent)
        .addProperty("releaseDataAfterLoad", &Texture::isReleaseDataAfterLoad, &Texture::setReleaseDataAfterLoad)
        .addFunction("releaseData", &Texture::releaseData)
        .addFunction("empty", &Texture::empty)
        .addFunction("isFramebuffer", &Texture::isFramebuffer)
        .addFunction("isFramebufferOutdated", &Texture::isFramebufferOutdated)
        .addProperty("minFilter", &Texture::getMinFilter, &Texture::setMinFilter)
        .addProperty("magFilter", &Texture::getMagFilter, &Texture::setMagFilter)
        .addProperty("wrapU", &Texture::getWrapU, &Texture::setWrapU)
        .addProperty("wrapV", &Texture::getWrapV, &Texture::setWrapV)
        .addProperty("svgScale", &Texture::getSvgScale, &Texture::setSvgScale)
        .addProperty("alphaBorder", &Texture::getAlphaBorder, &Texture::setAlphaBorder)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextureData>("TextureData")
        .addConstructor<
            void(), 
            void(int, int, unsigned int, ColorFormat, int, void*), 
            void(const char*)>()
        .addFunction("loadTextureFromFile", &TextureData::loadTextureFromFile)
        .addFunction("releaseImageData", &TextureData::releaseImageData)
        .addFunction("hasAlpha", &TextureData::hasAlpha)
        .addFunction("resizePowerOfTwo", &TextureData::resizePowerOfTwo)
        .addFunction("fitPowerOfTwo", &TextureData::fitPowerOfTwo)
        .addFunction("resize", &TextureData::resize)
        .addFunction("crop", &TextureData::crop)
        .addFunction("fitSize", &TextureData::fitSize)
        .addFunction("flipVertical", &TextureData::flipVertical)
        .addFunction("getColorComponent", &TextureData::getColorComponent)
        .addProperty("dataOwned", &TextureData::getDataOwned, &TextureData::setDataOwned)
        .addProperty("svgScale", &TextureData::getSVGScale, &TextureData::setSVGScale)
        .addFunction("getWidth", &TextureData::getWidth)
        .addFunction("getHeight", &TextureData::getHeight)
        .addFunction("getOriginalWidth", &TextureData::getOriginalWidth)
        .addFunction("getOriginalHeight", &TextureData::getOriginalHeight)
        .addFunction("getSize", &TextureData::getSize)
        .addFunction("getColorFormat", &TextureData::getColorFormat)
        .addFunction("getChannels", &TextureData::getChannels)
        .addFunction("getData", &TextureData::getData)
        .addFunction("isTransparent", &TextureData::isTransparent)
        .addFunction("getMinNearestPowerOfTwo", &TextureData::getMinNearestPowerOfTwo)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // A load result, read-only on purpose
        .beginClass<TextureLoadResult>("TextureLoadResult")
        .addConstructor<void()>()
        .addProperty("id", &TextureLoadResult::id)
        .addProperty("state", &TextureLoadResult::state)
        .addProperty("errorMessage", &TextureLoadResult::errorMessage)
        .addProperty("data", &TextureLoadResult::data)
        .addFunction("__tostring", [] (const TextureLoadResult& result) {
            return "TextureLoadResult(id: " + result.id + ", state: " + std::to_string(static_cast<int>(result.state)) + ")";
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        // the stencil state of a Material (material.stencil.compare = ... edits the material's
        // own) or of a UI/points/lines object (a copy: set it back)
        .beginClass<MaterialStencil>("MaterialStencil")
        .addConstructor<void()>()
        .addProperty("compare", &MaterialStencil::compare, &MaterialStencil::compare)
        .addProperty("ref", &MaterialStencil::ref, &MaterialStencil::ref)
        .addProperty("readMask", &MaterialStencil::readMask, &MaterialStencil::readMask)
        .addProperty("writeMask", &MaterialStencil::writeMask, &MaterialStencil::writeMask)
        .addProperty("pass", &MaterialStencil::pass, &MaterialStencil::pass)
        .addProperty("fail", &MaterialStencil::fail, &MaterialStencil::fail)
        .addProperty("depthFail", &MaterialStencil::depthFail, &MaterialStencil::depthFail)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // A Material's extra draw (material.extraPass, edited in place on the Material copy,
        // then mesh:setMaterial(m)): its own shader, parameters, textures, features and state.
        // An empty shader means no extra pass.
        .beginClass<MaterialPass>("MaterialPass")
        .addConstructor<void (*) (void)>()
        .addProperty("shader", &MaterialPass::shader, &MaterialPass::shader)
        .addProperty("blendMode", &MaterialPass::blendMode, &MaterialPass::blendMode)
        .addProperty("cullMode", &MaterialPass::cullMode, &MaterialPass::cullMode)
        .addProperty("depthWrite", &MaterialPass::depthWrite, &MaterialPass::depthWrite)
        .addProperty("depthTest", &MaterialPass::depthTest, &MaterialPass::depthTest)
        .addProperty("depthOffsetFactor", &MaterialPass::depthOffsetFactor, &MaterialPass::depthOffsetFactor)
        .addProperty("depthOffsetUnits", &MaterialPass::depthOffsetUnits, &MaterialPass::depthOffsetUnits)
        .addProperty("stencil", &MaterialPass::stencil, &MaterialPass::stencil)
        // MaterialColorWrite.AUTO (-1), NONE (0) or a sum of R, G, B, A
        .addProperty("colorWrite", &MaterialPass::colorWrite, &MaterialPass::colorWrite)
        .addProperty("order", &MaterialPass::order, &MaterialPass::order)
        // a @property value of the pass's shader (colours linear); unset reads the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<MaterialPass*, const std::string&, const Vector4&>(+[](MaterialPass* self, const std::string& name, const Vector4& value) {
                ShaderUniforms::set(self->shaderParams, name, value);
            }),
            luabridge::overload<MaterialPass*, const std::string&, const Vector3&>(+[](MaterialPass* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                ShaderUniforms::set(self->shaderParams, name, LuaBinding::getShaderParamValue(self->shader, name, value));
            }),
            luabridge::overload<MaterialPass*, const std::string&, const Vector2&>(+[](MaterialPass* self, const std::string& name, const Vector2& value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value.x, value.y, 0.0f, 0.0f));
            }),
            luabridge::overload<MaterialPass*, const std::string&, float>(+[](MaterialPass* self, const std::string& name, float value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value, 0.0f, 0.0f, 0.0f));
            }))
        .addFunction("getShaderParam", +[](const MaterialPass* self, const std::string& name) -> Vector4 {
            return ShaderLibraryValues::getParam(self->shaderParams, self->shader, name);
        })
        .addFunction("removeShaderParam", +[](MaterialPass* self, const std::string& name) -> bool {
            return ShaderUniforms::remove(self->shaderParams, name);
        })
        // the texture of a @texture uniform by path ("" removes it: the manifest default binds)
        .addFunction("setShaderTexture", +[](MaterialPass* self, const std::string& name, const std::string& path) {
            ShaderLibraryValues::setTexture(self->shaderTextures, name, path);
        })
        // a cube map (a @texture ... cube uniform): one image of the six faces, or six images
        .addFunction("setShaderCubeTexture",
            luabridge::overload<MaterialPass*, const std::string&, const std::string&>(+[](MaterialPass* self, const std::string& name, const std::string& path) {
                ShaderLibraryValues::setCubeTexture(self->shaderTextures, name, path);
            }),
            luabridge::overload<MaterialPass*, const std::string&, const std::string&, const std::string&, const std::string&,
                                const std::string&, const std::string&, const std::string&>(+[](MaterialPass* self, const std::string& name,
                    const std::string& front, const std::string& back, const std::string& left, const std::string& right, const std::string& up, const std::string& down) {
                ShaderLibraryValues::setCubeTextureFaces(self->shaderTextures, name, front, back, left, right, up, down);
            }))
        .addFunction("getShaderTexture", +[](const MaterialPass* self, const std::string& name) -> std::string {
            return ShaderLibraryValues::getTexture(self->shaderTextures, name);
        })
        // a @feature on or off, or a @keywords value ("GROUP_VALUE") chosen
        .addFunction("setShaderFeature", +[](MaterialPass* self, const std::string& define, bool enabled) {
            ShaderLibraryValues::setFeature(self->shaderFeatures, self->shader, define, enabled);
        })
        .addFunction("isShaderFeatureEnabled", +[](const MaterialPass* self, const std::string& define) -> bool {
            return ShaderLibraryValues::isFeatureEnabled(self->shaderFeatures, self->shader, define);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        // Mesh::getMaterial hands back a copy: the values are written on it, then the copy is
        // set back (m.roughnessFactor = 0.2; mesh:setMaterial(m)). Textures stay read-only.
        .beginClass<Material>("Material")
        .addProperty("baseColorFactor", &Material::baseColorFactor, &Material::baseColorFactor)
        .addProperty("metallicFactor", &Material::metallicFactor, &Material::metallicFactor)
        .addProperty("roughnessFactor", &Material::roughnessFactor, &Material::roughnessFactor)
        .addProperty("alphaMode", &Material::alphaMode, &Material::alphaMode)
        .addProperty("lighting", &Material::lighting, &Material::lighting)
        .addProperty("alphaCutoff", &Material::alphaCutoff, &Material::alphaCutoff)
        .addProperty("foliageTransmission", &Material::foliageTransmission, &Material::foliageTransmission)
        .addProperty("emissiveFactor", &Material::emissiveFactor, &Material::emissiveFactor)
        // Stochastic Tiling of the built-in Lit's maps (no repetition); the four values shape it
        .addProperty("stochasticTiling", &Material::stochasticTiling, &Material::stochasticTiling)
        .addProperty("stochasticCellSize", &Material::stochasticCellSize, &Material::stochasticCellSize)
        .addProperty("stochasticBlendSharpness", &Material::stochasticBlendSharpness, &Material::stochasticBlendSharpness)
        .addProperty("stochasticContrast", &Material::stochasticContrast, &Material::stochasticContrast)
        .addProperty("stochasticRotation", &Material::stochasticRotation, &Material::stochasticRotation)
        .addProperty("emissiveStrength", &Material::emissiveStrength, &Material::emissiveStrength)
        // the built-in Lit's extended lobes and hex tiling, flat (MaterialPbr's table: the
        // Lua name of each field); their maps are read-only here, as the other textures
        PBR_LUA_PROPERTY("clearcoatEnabled", clearcoat, Clearcoat, enabled)
        PBR_LUA_PROPERTY("clearcoatFactor", clearcoat, Clearcoat, factor)
        PBR_LUA_PROPERTY("clearcoatRoughness", clearcoat, Clearcoat, roughness)
        PBR_LUA_PROPERTY("clearcoatNormalScale", clearcoat, Clearcoat, normalScale)
        PBR_LUA_PROPERTY("sheenEnabled", sheen, Sheen, enabled)
        PBR_LUA_PROPERTY("sheenColor", sheen, Sheen, color)
        PBR_LUA_PROPERTY("sheenRoughness", sheen, Sheen, roughness)
        PBR_LUA_PROPERTY("anisotropyEnabled", anisotropy, Anisotropy, enabled)
        PBR_LUA_PROPERTY("anisotropyStrength", anisotropy, Anisotropy, strength)
        PBR_LUA_PROPERTY("anisotropyRotation", anisotropy, Anisotropy, rotation)
        PBR_LUA_PROPERTY("iridescenceEnabled", iridescence, Iridescence, enabled)
        PBR_LUA_PROPERTY("iridescenceFactor", iridescence, Iridescence, factor)
        PBR_LUA_PROPERTY("iridescenceIor", iridescence, Iridescence, ior)
        PBR_LUA_PROPERTY("iridescenceThicknessRange", iridescence, Iridescence, thicknessNm)
        PBR_LUA_PROPERTY("transmissionEnabled", transmission, Transmission, enabled)
        PBR_LUA_PROPERTY("transmissionFactor", transmission, Transmission, factor)
        PBR_LUA_PROPERTY("volumeEnabled", volume, Volume, enabled)
        PBR_LUA_PROPERTY("thicknessFactor", volume, Volume, thickness)
        PBR_LUA_PROPERTY("attenuationColor", volume, Volume, attenuationColor)
        PBR_LUA_PROPERTY("attenuationDistance", volume, Volume, attenuationDistance)
        PBR_LUA_PROPERTY("specularEnabled", specular, Specular, enabled)
        PBR_LUA_PROPERTY("specularFactor", specular, Specular, factor)
        PBR_LUA_PROPERTY("specularColor", specular, Specular, color)
        PBR_LUA_PROPERTY("ior", specular, Specular, ior)
        PBR_LUA_PROPERTY("hexTilingEnabled", hex, Hex, enabled)
        PBR_LUA_PROPERTY("hexTilingRotation", hex, Hex, rotation)
        PBR_LUA_PROPERTY("hexTilingContrast", hex, Hex, contrast)
        // the Hair shading model (S57)
        .addProperty("shadingModel", &materialShadingModel, &setMaterialShadingModel)
        PBR_LUA_PROPERTY("hairPrimaryColor", hair, Hair, primaryColor)
        PBR_LUA_PROPERTY("hairPrimaryShift", hair, Hair, primaryShift)
        PBR_LUA_PROPERTY("hairPrimaryWidth", hair, Hair, primaryWidth)
        PBR_LUA_PROPERTY("hairSecondaryColor", hair, Hair, secondaryColor)
        PBR_LUA_PROPERTY("hairSecondaryShift", hair, Hair, secondaryShift)
        PBR_LUA_PROPERTY("hairSecondaryWidth", hair, Hair, secondaryWidth)
        PBR_LUA_PROPERTY("hairRotation", hair, Hair, rotation)
        PBR_LUA_TEXTURE("clearcoatTexture", clearcoat, Clearcoat, map)
        PBR_LUA_TEXTURE("clearcoatNormalTexture", clearcoat, Clearcoat, normalMap)
        PBR_LUA_TEXTURE("sheenTexture", sheen, Sheen, map)
        PBR_LUA_TEXTURE("anisotropyTexture", anisotropy, Anisotropy, map)
        PBR_LUA_TEXTURE("iridescenceTexture", iridescence, Iridescence, map)
        PBR_LUA_TEXTURE("transmissionTexture", transmission, Transmission, map)
        PBR_LUA_TEXTURE("specularTexture", specular, Specular, map)
        PBR_LUA_TEXTURE("hairFlowTexture", hair, Hair, flowMap)
        .addProperty("baseColorTexture", &Material::baseColorTexture)
        .addProperty("emissiveTexture", &Material::emissiveTexture)
        .addProperty("metallicRoughnessTexture", &Material::metallicRoughnessTexture)
        .addProperty("occlusionTexture", &Material::occlusionTexture)
        .addProperty("normalTexture", &Material::normalTexture)
        // shader library, writable on the copy getMaterial gives (then setMaterial it):
        // shader is "" (built-in Lit), "library:<name>" or a project fork
        .addProperty("shader", &Material::shader, &Material::shader)
        .addProperty("blendMode", &Material::blendMode, &Material::blendMode)
        .addProperty("cullMode", &Material::cullMode, &Material::cullMode)
        .addProperty("depthWrite", &Material::depthWrite, &Material::depthWrite)
        .addProperty("depthTest", &Material::depthTest, &Material::depthTest)
        .addProperty("depthOffsetFactor", &Material::depthOffsetFactor, &Material::depthOffsetFactor)
        .addProperty("depthOffsetUnits", &Material::depthOffsetUnits, &Material::depthOffsetUnits)
        .addProperty("sortPriority", &Material::sortPriority, &Material::sortPriority)
        .addProperty("stencil", &Material::stencil, &Material::stencil)
        .addProperty("colorWrite", &Material::colorWrite, &Material::colorWrite)
        // one optional extra draw of the submesh (outlines, masks), edited in place:
        // m.extraPass.shader = "library:outline"; mesh:setMaterial(m)
        .addProperty("extraPass",
            // the material's own pass (made on first access): m.extraPass.order = ... edits it
            +[](const Material* self) -> MaterialPass* { return &const_cast<Material*>(self)->extraPass.edit(); },
            +[](Material* self, const MaterialPass& pass) { self->extraPass = pass; })
        // a @property value of the shader (colours linear); unset reads the manifest default.
        // The shader is the material's, else the one its submesh draws with (a copy from
        // Mesh:getMaterial remembers it: the mesh's customShader or the scene default).
        .addFunction("setShaderParam",
            luabridge::overload<Material*, const std::string&, const Vector4&>(+[](Material* self, const std::string& name, const Vector4& value) {
                ShaderUniforms::set(self->shaderParams, name, value);
            }),
            luabridge::overload<Material*, const std::string&, const Vector3&, lua_State*>(+[](Material* self, const std::string& name, const Vector3& value, lua_State* L) {
                // a colour given as (r, g, b) is opaque
                const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
                ShaderUniforms::set(self->shaderParams, name, LuaBinding::getShaderParamValue(shader, name, value));
            }),
            luabridge::overload<Material*, const std::string&, const Vector2&>(+[](Material* self, const std::string& name, const Vector2& value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value.x, value.y, 0.0f, 0.0f));
            }),
            luabridge::overload<Material*, const std::string&, float>(+[](Material* self, const std::string& name, float value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value, 0.0f, 0.0f, 0.0f));
            }))
        .addFunction("getShaderParam", +[](const Material* self, const std::string& name, lua_State* L) -> Vector4 {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            return ShaderLibraryValues::getParam(self->shaderParams, shader, name);
        })
        .addFunction("removeShaderParam", +[](Material* self, const std::string& name) -> bool {
            return ShaderUniforms::remove(self->shaderParams, name);
        })
        // a @property member that follows a global shader value (Shader.setGlobal*) instead of
        // its own value; nil unbinds (then mesh:setMaterial(m))
        .addFunction("setShaderGlobalBinding", +[](Material* self, const std::string& member, luabridge::LuaRef global) -> bool {
            return ShaderGlobals::setBinding(self->shaderGlobalBindings, member, luaBindingGlobal(global, "Material.setShaderGlobalBinding"));
        })
        .addFunction("getShaderGlobalBinding", +[](const Material* self, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = ShaderGlobals::getBinding(self->shaderGlobalBindings, member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
        // the texture of a @texture uniform by path ("" removes it: the manifest default binds)
        .addFunction("setShaderTexture", +[](Material* self, const std::string& name, const std::string& path) {
            ShaderLibraryValues::setTexture(self->shaderTextures, name, path);
        })
        // a cube map (a @texture ... cube uniform): one image of the six faces, or six images
        .addFunction("setShaderCubeTexture",
            luabridge::overload<Material*, const std::string&, const std::string&>(+[](Material* self, const std::string& name, const std::string& path) {
                ShaderLibraryValues::setCubeTexture(self->shaderTextures, name, path);
            }),
            luabridge::overload<Material*, const std::string&, const std::string&, const std::string&, const std::string&,
                                const std::string&, const std::string&, const std::string&>(+[](Material* self, const std::string& name,
                    const std::string& front, const std::string& back, const std::string& left, const std::string& right, const std::string& up, const std::string& down) {
                ShaderLibraryValues::setCubeTextureFaces(self->shaderTextures, name, front, back, left, right, up, down);
            }))
        .addFunction("getShaderTexture", +[](const Material* self, const std::string& name) -> std::string {
            return ShaderLibraryValues::getTexture(self->shaderTextures, name);
        })
        // a @feature on or off, or a @keywords value ("GROUP_VALUE") chosen
        .addFunction("setShaderFeature", +[](Material* self, const std::string& define, bool enabled, lua_State* L) {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            ShaderLibraryValues::setFeature(self->shaderFeatures, shader, define, enabled);
        })
        .addFunction("isShaderFeatureEnabled", +[](const Material* self, const std::string& define, lua_State* L) -> bool {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            return ShaderLibraryValues::isFeatureEnabled(self->shaderFeatures, shader, define);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Framebuffer>("Framebuffer")
        .addConstructor <void (*) (void)> ()
        .addFunction("create", luabridge::overload<>(&Framebuffer::create))
        .addFunction("destroy", &Framebuffer::destroy)
        .addFunction("isCreated", &Framebuffer::isCreated)
        .addFunction("getRender", &Framebuffer::getRender)
        .addFunction("getVersion", &Framebuffer::getVersion)
        .addFunction("create", luabridge::overload<>(&Framebuffer::create))
        .addFunction("width", &Framebuffer::getWidth, &Framebuffer::setWidth)
        .addFunction("height", &Framebuffer::getHeight, &Framebuffer::setHeight)
        .addProperty("minFilter", &Framebuffer::getMinFilter, &Framebuffer::setMinFilter)
        .addProperty("magFilter", &Framebuffer::getMagFilter, &Framebuffer::setMagFilter)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextureRender>("TextureRender")
        .addConstructor <void (*) (void)> ()
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FramebufferRender>("FramebufferRender")
        .addConstructor <void (*) (void)> ()
        .addFunction("createFramebuffer", luabridge::overload<TextureType, int, int, TextureFilter, TextureFilter, TextureWrap, TextureWrap, bool>(&FramebufferRender::createFramebuffer))
        .addFunction("destroyFramebuffer", &FramebufferRender::destroyFramebuffer)
        .addFunction("isCreated", &FramebufferRender::isCreated)
        .addFunction("getColorTexture", &FramebufferRender::getColorTexture)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<EntityRegistry>("EntityRegistry")
        .addConstructor <void (*) (void), void (*) (EntityPool)> ()
        .addFunction("createEntity", &EntityRegistry::createEntity)
        .addFunction("createUserEntity", &EntityRegistry::createUserEntity)
        .addFunction("createSystemEntity", &EntityRegistry::createSystemEntity)
        .addFunction("recreateEntity", &EntityRegistry::recreateEntity)
        .addFunction("isEntityCreated", &EntityRegistry::isEntityCreated)
        .addFunction("destroyEntity", &EntityRegistry::destroyEntity)
        .addFunction("setEntityName", &EntityRegistry::setEntityName)
        .addFunction("getEntityName", &EntityRegistry::getEntityName)
        .addFunction("findEntity",
            luabridge::constOverload<const std::string&>(&EntityRegistry::findEntity),
            luabridge::constOverload<const std::string&, Entity>(&EntityRegistry::findEntity))
        .addFunction("getSignature", &EntityRegistry::getSignature)
        .addFunction("setEntityActive", &EntityRegistry::setEntityActive)
        .addFunction("isEntityActive", &EntityRegistry::isEntityActive)
        .addFunction("isEntityActiveInHierarchy", &EntityRegistry::isEntityActiveInHierarchy)
        // scripts get Unity's check; loaders and generated scenes call the C++ setter directly
        .addFunction("setEntityTag", [] (EntityRegistry* self, Entity entity, const std::string& tag) {
            if (!TagManager::isTagDefined(tag)){
                Log::error("Tag: %s is not defined.", tag.c_str());
                return;
            }
            self->setEntityTag(entity, tag);
        })
        .addFunction("getEntityTag", &EntityRegistry::getEntityTag)
        .addFunction("setEntityLayer", &EntityRegistry::setEntityLayer)
        .addFunction("getEntityLayer", &EntityRegistry::getEntityLayer)
        .addFunction("findEntityByTag", &EntityRegistry::findEntityByTag)
        .addFunction("findEntitiesByTag", &EntityRegistry::findEntitiesByTag)
        // Unity-style names: GameObject.FindWithTag / FindGameObjectsWithTag
        .addFunction("findByTag", &EntityRegistry::findEntityByTag)
        .addFunction("findAllByTag", &EntityRegistry::findEntitiesByTag)
        .addFunction("addEntityChild", &EntityRegistry::addEntityChild)
        .addFunction("moveChildToIndex", &EntityRegistry::moveChildToIndex)
        .addFunction("moveChildToTop", &EntityRegistry::moveChildToTop)
        .addFunction("moveChildUp", &EntityRegistry::moveChildUp)
        .addFunction("moveChildDown", &EntityRegistry::moveChildDown)
        .addFunction("moveChildToBottom", &EntityRegistry::moveChildToBottom)
        .addFunction("findOldestParent", &EntityRegistry::findOldestParent)
        .addFunction("isParentOf", &EntityRegistry::isParentOf)
        .addFunction("findBranchLastIndex", &EntityRegistry::findBranchLastIndex)
        .addFunction("setDefaultEntityPool", &EntityRegistry::setDefaultEntityPool)
        .addFunction("getDefaultEntityPool", &EntityRegistry::getDefaultEntityPool)
        .addFunction("getLastEntity", &EntityRegistry::getLastEntity)
        .addFunction("getEntityList", &EntityRegistry::getEntityList)
        .addFunction("clear", &EntityRegistry::clear)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Scene, EntityRegistry>("Scene")
        .addConstructor <void (*) (void), void (*) (EntityPool)> ()
        .addFunction("load", &Scene::load)
        .addFunction("destroy", &Scene::destroy)
        .addFunction("draw", &Scene::draw)
        .addFunction("update", &Scene::update)
        .addFunction("updateCameraSize", &Scene::updateCameraSize)
        .addProperty("camera", (Entity(Scene::*)()const)&Scene::getCamera, (void(Scene::*)(Entity))&Scene::setCamera)
        .addFunction("setCamera", 
            luabridge::overload<Camera*>(&Scene::setCamera),
            luabridge::overload<Entity>(&Scene::setCamera))
        .addProperty("backgroundColor", &Scene::getBackgroundColor, (void (Scene::*)(Vector4))&Scene::setBackgroundColor)
        .addFunction("setBackgroundColor", 
            luabridge::overload<float, float, float>(&Scene::setBackgroundColor),
            luabridge::overload<float, float, float, float>(&Scene::setBackgroundColor))
        .addProperty("shadowQuality", &Scene::getShadowQuality, &Scene::setShadowQuality)
        .addProperty("shadowDistance", &Scene::getShadowDistance, &Scene::setShadowDistance)
        .addProperty("shadowFadeFraction", &Scene::getShadowFadeFraction, &Scene::setShadowFadeFraction)
        .addProperty("shadowCascadeBlend", &Scene::isShadowCascadeBlend, &Scene::setShadowCascadeBlend)
        .addProperty("effectiveShadowTier", &Scene::getEffectiveShadowTier)
        .addProperty("lightState", &Scene::getLightState, &Scene::setLightState)
        .addProperty("globalIlluminationColor", &Scene::getGlobalIlluminationColor, (void (Scene::*)(Vector3))&Scene::setGlobalIllumination)
        .addProperty("globalIlluminationIntensity", &Scene::getGlobalIlluminationIntensity, (void (Scene::*)(float))&Scene::setGlobalIllumination)
        .addFunction("getGlobalIlluminationColorLinear", &Scene::getGlobalIlluminationColorLinear)
        .addFunction("setGlobalIllumination", (void (Scene::*)(float, Vector3))&Scene::setGlobalIllumination)
        .addProperty("maxLightsPerObject", &Scene::getMaxLightsPerObject, &Scene::setMaxLightsPerObject)
        // the built-in Lit's quality (LitQuality.STANDARD, HIGH) and the exposure lights in
        // physical units are drawn at (EV100)
        .addProperty("litQuality", &Scene::getLitQuality, &Scene::setLitQuality)
        .addProperty("physicalLightEV100", &Scene::getPhysicalLightEV100, &Scene::setPhysicalLightEV100)
        .addProperty("capsuleShadowsEnabled", &Scene::isCapsuleShadowsEnabled, &Scene::setCapsuleShadowsEnabled)
        .addProperty("capsuleShadowIntensity", &Scene::getCapsuleShadowIntensity, &Scene::setCapsuleShadowIntensity)
        .addProperty("capsuleShadowSoftness", &Scene::getCapsuleShadowSoftness, &Scene::setCapsuleShadowSoftness)
        .addProperty("ambientLight2DColor", &Scene::getAmbientLight2DColor, (void (Scene::*)(Vector3))&Scene::setAmbientLight2D)
        .addProperty("ambientLight2DIntensity", &Scene::getAmbientLight2DIntensity, (void (Scene::*)(float))&Scene::setAmbientLight2D)
        .addFunction("getAmbientLight2DColorLinear", &Scene::getAmbientLight2DColorLinear)
        .addFunction("setAmbientLight2D", (void (Scene::*)(float, Vector3))&Scene::setAmbientLight2D)
        .addProperty("shadow2DQuality", &Scene::getShadow2DQuality, &Scene::setShadow2DQuality)
        .addProperty("gravity2D", &Scene::getGravity2D, (void (Scene::*)(Vector2))&Scene::setGravity2D)
        .addFunction("setGravity2D",
            luabridge::overload<Vector2>(&Scene::setGravity2D),
            luabridge::overload<float, float>(&Scene::setGravity2D))
        .addProperty("gravity3D", &Scene::getGravity3D, (void (Scene::*)(Vector3))&Scene::setGravity3D)
        .addFunction("setGravity3D",
            luabridge::overload<Vector3>(&Scene::setGravity3D),
            luabridge::overload<float, float, float>(&Scene::setGravity3D))
        .addProperty("physicsInterpolation", &Scene::isPhysicsInterpolation, &Scene::setPhysicsInterpolation)
        .addProperty("physics2DSubSteps", &Scene::getPhysics2DSubSteps, &Scene::setPhysics2DSubSteps)
        .addProperty("ssaoEnabled", &Scene::isSSAOEnabled, &Scene::setSSAOEnabled)
        .addProperty("ssaoRadius", &Scene::getSSAORadius, &Scene::setSSAORadius)
        .addProperty("ssaoIntensity", &Scene::getSSAOIntensity, &Scene::setSSAOIntensity)
        .addProperty("ssaoBias", &Scene::getSSAOBias, &Scene::setSSAOBias)
        .addProperty("meshLodEnabled", &Scene::isMeshLodEnabled, &Scene::setMeshLodEnabled)
        .addProperty("meshLodThreshold", &Scene::getMeshLodThreshold, &Scene::setMeshLodThreshold)
        // the last frame's render statistics (spec S55 R7), read-only: draws, triangles and
        // instances of every pass and by kind of pass, the GPU Instancing batches, what the
        // visible lists kept and culled, uniform and upload bytes
        .addFunction("getRenderStats", +[](const Scene*, lua_State* L) -> luabridge::LuaRef {
            const RenderStats stats = SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME);
            luabridge::LuaRef table = luabridge::newTable(L);
            table["drawCalls"] = stats.drawCalls;
            table["instances"] = stats.instances;
            table["triangles"] = (double)stats.triangles;
            table["mainTriangles"] = (double)stats.mainTriangles;
            table["shadowTriangles"] = (double)stats.shadowTriangles;
            table["mainDrawCalls"] = stats.mainDraws;
            table["shadowDrawCalls"] = stats.shadowDraws;
            table["depthDrawCalls"] = stats.depthDraws;
            table["otherDrawCalls"] = stats.otherDraws;
            table["instancedDraws"] = stats.instancedDraws;
            table["batchedInstances"] = stats.batchedInstances;
            table["visible"] = stats.visible;
            table["culled"] = stats.culled;
            table["uniformBytes"] = stats.uniformBytes;
            table["bufferUploadBytes"] = stats.bufferUploadBytes;
            return table;
        })
        .addProperty("crossFade", &Scene::isMeshLodCrossFade, &Scene::setMeshLodCrossFade)
        .addProperty("crossFadeDuration", &Scene::getMeshLodCrossFadeDuration, &Scene::setMeshLodCrossFadeDuration)
        .addProperty("depthPrepassEnabled", &Scene::isDepthPrepassEnabled, &Scene::setDepthPrepassEnabled)
        .addProperty("sceneColorScale", &Scene::getSceneColorScale, &Scene::setSceneColorScale)
        // water planar reflections (library:water's PLANAR_REFLECTION; a quality menu sets OFF)
        .addProperty("planarReflectionResolution", &Scene::getPlanarReflectionResolution, &Scene::setPlanarReflectionResolution)
        .addProperty("planarReflectionLayers", &Scene::getPlanarReflectionLayers, &Scene::setPlanarReflectionLayers)
        .addProperty("planarReflectionShadows", &Scene::getPlanarReflectionShadows, &Scene::setPlanarReflectionShadows)
        .addProperty("planarReflectionFarClip", &Scene::getPlanarReflectionFarClip, &Scene::setPlanarReflectionFarClip)
        .addProperty("planarReflectionClipOffset", &Scene::getPlanarReflectionClipOffset, &Scene::setPlanarReflectionClipOffset)
        .addProperty("maxPlanarReflections", &Scene::getMaxPlanarReflections, &Scene::setMaxPlanarReflections)
        .addProperty("waterWakeResolution", &Scene::getWaterWakeResolution, &Scene::setWaterWakeResolution)
        .addProperty("waterWakeArea", &Scene::getWaterWakeArea, &Scene::setWaterWakeArea)
        .addProperty("waterWakeFade", &Scene::getWaterWakeFade, &Scene::setWaterWakeFade)
        // a copy (edit it and assign it back, which applies it)
        .addProperty("postProcessing", [] (const Scene* self) { return PostProcessingSettings(self->getPostProcessing()); }, &Scene::setPostProcessing)
        .addProperty("ssaoDebug", &Scene::isSSAODebug, &Scene::setSSAODebug)
        .addProperty("ssrEnabled", &Scene::isSSREnabled, &Scene::setSSREnabled)
        .addProperty("ssrMaxDistance", &Scene::getSSRMaxDistance, &Scene::setSSRMaxDistance)
        .addProperty("ssrThickness", &Scene::getSSRThickness, &Scene::setSSRThickness)
        .addProperty("ssrMaxSteps", &Scene::getSSRMaxSteps, &Scene::setSSRMaxSteps)
        .addProperty("ssrIntensity", &Scene::getSSRIntensity, &Scene::setSSRIntensity)
        .addProperty("ssrBlur", &Scene::getSSRBlur, &Scene::setSSRBlur)
        .addProperty("ssrDebugMode", &Scene::getSSRDebugMode, &Scene::setSSRDebugMode)
        .addProperty("fixedResolutionEnabled", &Scene::isFixedResolutionEnabled, &Scene::setFixedResolutionEnabled)
        .addProperty("fixedResolutionWidth", &Scene::getFixedResolutionWidth, &Scene::setFixedResolutionWidth)
        .addProperty("fixedResolutionHeight", &Scene::getFixedResolutionHeight, &Scene::setFixedResolutionHeight)
        .addProperty("fixedResolutionFilter", &Scene::getFixedResolutionFilter, &Scene::setFixedResolutionFilter)
        .addFunction("setFixedResolutionSize", &Scene::setFixedResolutionSize)
        .addFunction("getCanvasScaler", [] (Scene* self) { return CanvasScalerRef(self); })
        .addProperty("defaultMeshShader", &Scene::getDefaultMeshShader, &Scene::setDefaultMeshShader)
        .addProperty("defaultUIShader", &Scene::getDefaultUIShader, &Scene::setDefaultUIShader)
        .addProperty("defaultSkyShader", &Scene::getDefaultSkyShader, &Scene::setDefaultSkyShader)
        .addProperty("defaultPointsShader", &Scene::getDefaultPointsShader, &Scene::setDefaultPointsShader)
        .addProperty("defaultLinesShader", &Scene::getDefaultLinesShader, &Scene::setDefaultLinesShader)
        // property values of a default shader, inherited by the objects that use it
        .addFunction("setDefaultShaderUniform",
            luabridge::overload<ShaderType, const std::string&, const Vector4&>(&Scene::setDefaultShaderUniform),
            luabridge::overload<ShaderType, const std::string&, const Vector3&>(&Scene::setDefaultShaderUniform),
            luabridge::overload<ShaderType, const std::string&, const Vector2&>(&Scene::setDefaultShaderUniform),
            luabridge::overload<ShaderType, const std::string&, float>(&Scene::setDefaultShaderUniform))
        .addFunction("getDefaultShaderUniform", &Scene::getDefaultShaderUniform)
        .addFunction("hasDefaultShaderUniform", &Scene::hasDefaultShaderUniform)
        .addFunction("removeDefaultShaderUniform", &Scene::removeDefaultShaderUniform)
        .addProperty("postProcessPasses", &Scene::getPostProcessPasses, &Scene::setPostProcessPasses)
        .addFunction("setPostProcessUniform",
            luabridge::overload<unsigned int, const std::string&, const Vector4&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, const Vector3&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, const Vector2&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, float>(&Scene::setPostProcessUniform))
        .addFunction("getPostProcessUniform", &Scene::getPostProcessUniform)
        .addFunction("removePostProcessUniform", &Scene::removePostProcessUniform)
        // a pass's u_fs_postParams member that follows a global shader value; nil unbinds
        .addFunction("setPostProcessGlobalBinding", +[](Scene* self, unsigned int index, const std::string& member, luabridge::LuaRef global) -> bool {
            return self->setPostProcessGlobalBinding(index, member, luaBindingGlobal(global, "Scene.setPostProcessGlobalBinding"));
        })
        .addFunction("getPostProcessGlobalBinding", +[](const Scene* self, unsigned int index, const std::string& member, lua_State* L) -> luabridge::LuaRef {
            const std::string global = self->getPostProcessGlobalBinding(index, member);
            return global.empty() ? luabridge::LuaRef(L) : luabridge::LuaRef(L, global);
        })
        .addFunction("setPostProcessPassEnabled", &Scene::setPostProcessPassEnabled)
        .addFunction("isPostProcessPassEnabled", &Scene::isPostProcessPassEnabled)
        .addFunction("canReceiveUIEvents", &Scene::canReceiveUIEvents)
        .addProperty("enableUIEvents", &Scene::getEnableUIEvents, (void (Scene::*)(UIEventState))&Scene::setEnableUIEvents)
        .addFunction("getActionSystem", [] (Scene* self, lua_State* L) { return self->getSystem<ActionSystem>().get(); })
        .addFunction("getAudioSystem", [] (Scene* self, lua_State* L) { return self->getSystem<AudioSystem>().get(); })
        .addFunction("getMeshSystem", [] (Scene* self, lua_State* L) { return self->getSystem<MeshSystem>().get(); })
        .addFunction("getPhysicsSystem", [] (Scene* self, lua_State* L) { return self->getSystem<PhysicsSystem>().get(); })
        .addFunction("getRenderSystem", [] (Scene* self, lua_State* L) { return self->getSystem<RenderSystem>().get(); })
        // dynamic resolution as the renderer applies it (S13): {enabled, running, overridden, scale,
        // minScale, maxScale, source ("gpu", "frame", "none"), smoothedMs, targetMs, saturatedLow,
        // ineffective, acceptedChanges, probeSeconds}
        .addFunction("getDynamicResolution", [] (Scene* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            std::shared_ptr<RenderSystem> render = self->getSystem<RenderSystem>();
            const DynamicResolutionStatus status = render ? render->getDynamicResolutionStatus() : DynamicResolutionStatus();
            table["enabled"] = status.enabled;
            table["running"] = status.running;
            table["idle"] = std::string(status.idle);
            table["overridden"] = status.overridden;
            table["scale"] = status.scale;
            table["minScale"] = status.minScale;
            table["maxScale"] = status.maxScale;
            table["source"] = std::string(status.source);
            table["smoothedMs"] = status.smoothedMs;
            table["targetMs"] = status.targetMs;
            table["saturatedLow"] = status.saturatedLow;
            table["ineffective"] = status.ineffective;
            table["acceptedChanges"] = (double)status.acceptedChanges;
            table["probeSeconds"] = status.probeSeconds;
            return table;
        })
        // pins the main camera's render scale (0.5..1) over dynamic resolution; nil releases it
        .addFunction("setDynamicResolutionOverride", [] (Scene* self, std::optional<float> scale) {
            if (std::shared_ptr<RenderSystem> render = self->getSystem<RenderSystem>())
                render->setDynamicResolutionOverride(scale ? *scale : 0.0f);
        })
        .addFunction("getUISystem", [] (Scene* self, lua_State* L) { return self->getSystem<UISystem>().get(); })
        // change tracking: the tick a change made from now on is newer than
        .addFunction("getChangeTick", [] (Scene* self, lua_State* L) -> lua_Integer { return (lua_Integer)self->changeTick(); })
        // findEntitiesWith({"Transform", "MeshComponent"}[, {"UIComponent"}]): ids in the
        // order of the first type's array (parents before children for Transform)
        .addFunction("findEntitiesWith", [] (Scene* self, lua_State* L) -> std::vector<Entity> {
            std::vector<ComponentId> include, exclude;
            if (!luaComponentIds(L, 2, self, "findEntitiesWith", include) || !luaComponentIds(L, 3, self, "findEntitiesWith", exclude)){
                return {};
            }
            return self->queryEntities(include, exclude);
        })
        // findChangedEntities("Transform", sinceTick): those whose component changed after it
        .addFunction("findChangedEntities", [] (Scene* self, lua_State* L) -> std::vector<Entity> {
            ComponentId id;
            if (!luaComponentId(L, 2, self, "findChangedEntities", id)){
                return {};
            }
            const Tick since = (Tick)std::max<lua_Integer>(0, luaL_optinteger(L, 3, 0));
            return self->queryEntities({id}, {}, &since);
        })
        // markChanged(entity, "MeshComponent"): false when the entity has no such component
        .addFunction("markChanged", [] (Scene* self, lua_State* L) -> bool {
            ComponentId id;
            if (!luaComponentId(L, 3, self, "markChanged", id)){
                return false;
            }
            return self->markChanged(id, (Entity)luaL_checkinteger(L, 2));
        })
        .endClass();

    // Log.*(...): category lua, any values (tab-joined), with the calling chunk and line and a
    // traceback per Log Stack Traces (luaLogLine). debug is compiled out of release builds as
    // before; Log.category(name) gives a category's own trace...error
    luabridge::getGlobalNamespace(L)
        .beginClass<Log>("Log")
        .addStaticFunction("print", &luaLogPrint)
        .addStaticFunction("info", &luaLogPrint)
        .addStaticFunction("verbose", &luaLogVerbose)
        .addStaticFunction("debug", &luaLogDebug)
        .addStaticFunction("warn", &luaLogWarn)
        .addStaticFunction("error", &luaLogError)
        // false (and a warning) for an unknown level name
        .addStaticFunction("setLevel", [] (const std::string& level, std::optional<std::string> category, lua_State* L) {
            LogLevel value;
            if (!parseLogLevel(level.c_str(), value)) {
                luaLog(L, LogLevel::Warn, logCategoryLua(),
                       ("Log.setLevel: unknown level '" + level + "' (trace, debug, info, warn, error, fatal, off)").c_str());
                return false;
            }
            Logger::setCategoryLevel(category ? Logger::registerCategory(*category, LogLevel::Info) : logCategoryLua(), value);
            return true;
        })
        .addStaticFunction("getLevel", [] (std::optional<std::string> category) -> std::string {
            const LogCategory* found = category ? Logger::findCategory(*category) : &logCategoryLua();
            return logLevelName(found ? Logger::categoryLevel(*found) : Logger::defaultLevel());
        })
        .addStaticFunction("category", [] (const std::string& name) {
            return LuaLogCategory{&Logger::registerCategory(name, LogLevel::Info)};
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<LuaLogCategory>("LogCategory")
        .addProperty("name", [] (const LuaLogCategory* self) { return std::string(self->category->name); })
        .addFunction("trace", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Trace, *self->category, text); })
        .addFunction("debug", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Debug, *self->category, text); })
        .addFunction("info", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Info, *self->category, text); })
        .addFunction("warn", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Warn, *self->category, text); })
        .addFunction("error", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Error, *self->category, text); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LogLevel")
        // the Logger's one LogLevel (S09 R1): TRACE and VERBOSE are the same level
        .addVariable("TRACE", LogLevel::Trace)
        .addVariable("VERBOSE", LogLevel::Trace)
        .addVariable("DEBUG", LogLevel::Debug)
        .addVariable("INFO", LogLevel::Info)
        .addVariable("WARN", LogLevel::Warn)
        .addVariable("ERROR", LogLevel::Error)
        .addVariable("FATAL", LogLevel::Fatal)
        .endNamespace();

    // Log.onMessageReceived's event: (message, stackTrace, level)
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(const std::string&, const std::string&, int)>>("FunctionSubscribe_V_SSI")
        .addFunction("__call", &FunctionSubscribe<void(const std::string&, const std::string&, int)>::call)
        .addFunction("call", &FunctionSubscribe<void(const std::string&, const std::string&, int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(const std::string&, const std::string&, int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(const std::string&, const std::string&, int)>::add)
        .addFunction("remove", &FunctionSubscribe<void(const std::string&, const std::string&, int)>::remove)
        .addFunction("has", &FunctionSubscribe<void(const std::string&, const std::string&, int)>::has)
        .endClass();

    // the engine's log (feature debug-overlay): the buffer and listeners, on S09's Log class
    // (re-opened: its print... and categories are S09's)
    luabridge::getGlobalNamespace(L)
        .beginClass<Log>("Log")
        // Unity's Application.logMessageReceived: RegisterEvent(self, Log.onMessageReceived, "onLog")
        .addStaticProperty("onMessageReceived", [] () { return &Log::onMessageReceived; }, [] (lua_State* L) { Log::onMessageReceived = L; })
        .addStaticFunction("getEntries", &luaLogGetEntries)
        .addStaticFunction("clear", &Log::clearEntries)
        .addStaticFunction("count", &luaLogCount)
        .addStaticProperty("bufferSize", [] () -> int { return (int)std::min<size_t>(Log::getBufferSize(), INT32_MAX); },
                           // 0 .. Log::maxBufferSize (100000), clamped
                           [] (int size) { Log::setBufferSize(size > 0 ? (size_t)size : 0); })
        .endClass();

    // the on-device console and stats overlay (Unity's IngameDebugConsole and Graphy)
    luabridge::getGlobalNamespace(L)
        .beginClass<DebugOverlay>("DebugOverlay")
        .addStaticProperty("enabled", &DebugOverlay::isEnabled, &DebugOverlay::setEnabled)
        .addStaticFunction("showConsole", &DebugOverlay::showConsole)
        .addStaticFunction("hideConsole", &DebugOverlay::hideConsole)
        .addStaticFunction("toggleConsole", &DebugOverlay::toggleConsole)
        .addStaticProperty("consoleVisible", &DebugOverlay::isConsoleVisible)
        .addStaticProperty("statsMode", &DebugOverlay::getStatsMode, static_cast<lua_CFunction>(&luaDebugOverlaySetStatsMode))
        .addStaticProperty("STATS_OFF", [] () -> int { return (int)DebugOverlaySettings::Stats::Off; })
        .addStaticProperty("STATS_FPS", [] () -> int { return (int)DebugOverlaySettings::Stats::Fps; })
        .addStaticProperty("STATS_FULL", [] () -> int { return (int)DebugOverlaySettings::Stats::Full; })
        .addStaticFunction("getStats", &luaDebugOverlayGetStats)
        .addStaticFunction("addCommand", &luaDebugOverlayAddCommand)
        .addStaticFunction("removeCommand", &DebugOverlay::removeCommand)
        .addStaticFunction("executeCommand", &DebugOverlay::executeCommand)
        .endClass();

    // Unity's TouchPhase (the values of Input.TOUCH_BEGAN ...)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("TouchPhase")
        .addVariable("Began", TouchPhase::Began)
        .addVariable("Moved", TouchPhase::Moved)
        .addVariable("Stationary", TouchPhase::Stationary)
        .addVariable("Ended", TouchPhase::Ended)
        .addVariable("Canceled", TouchPhase::Canceled)
        .endNamespace();

    // Input.anyKeyDown both as Unity's property and as S22's function
    luaMakeBooleansCallable(L);

    luabridge::getGlobalNamespace(L)
        .beginClass<Input>("Input")
        .addStaticProperty("MODIFIER_SHIFT", [] () -> int { return D_MODIFIER_SHIFT; })
        .addStaticProperty("MODIFIER_CONTROL", [] () -> int { return D_MODIFIER_CONTROL; })
        .addStaticProperty("MODIFIER_ALT", [] () -> int { return D_MODIFIER_ALT; })
        .addStaticProperty("MODIFIER_SUPER", [] () -> int { return D_MODIFIER_SUPER; })
        .addStaticProperty("MODIFIER_CAPS_LOCK", [] () -> int { return D_MODIFIER_CAPS_LOCK; })
        .addStaticProperty("MODIFIER_NUM_LOCK", [] () -> int { return D_MODIFIER_NUM_LOCK; })
        .addStaticProperty("KEY_UNKNOWN", [] () -> int { return D_KEY_UNKNOWN; })
        .addStaticProperty("KEY_SPACE", [] () -> int { return D_KEY_SPACE; })
        .addStaticProperty("KEY_APOSTROPHE", [] () -> int { return D_KEY_APOSTROPHE; })
        .addStaticProperty("KEY_COMMA", [] () -> int { return D_KEY_COMMA; })
        .addStaticProperty("KEY_MINUS", [] () -> int { return D_KEY_MINUS; })
        .addStaticProperty("KEY_PERIOD", [] () -> int { return D_KEY_PERIOD; })
        .addStaticProperty("KEY_SLASH", [] () -> int { return D_KEY_SLASH; })
        .addStaticProperty("KEY_0", [] () -> int { return D_KEY_0; })
        .addStaticProperty("KEY_1", [] () -> int { return D_KEY_1; })
        .addStaticProperty("KEY_2", [] () -> int { return D_KEY_2; })
        .addStaticProperty("KEY_3", [] () -> int { return D_KEY_3; })
        .addStaticProperty("KEY_4", [] () -> int { return D_KEY_4; })
        .addStaticProperty("KEY_5", [] () -> int { return D_KEY_5; })
        .addStaticProperty("KEY_6", [] () -> int { return D_KEY_6; })
        .addStaticProperty("KEY_7", [] () -> int { return D_KEY_7; })
        .addStaticProperty("KEY_8", [] () -> int { return D_KEY_8; })
        .addStaticProperty("KEY_9", [] () -> int { return D_KEY_9; })
        .addStaticProperty("KEY_SEMICOLON", [] () -> int { return D_KEY_SEMICOLON; })
        .addStaticProperty("KEY_EQUAL", [] () -> int { return D_KEY_EQUAL; })
        .addStaticProperty("KEY_A", [] () -> int { return D_KEY_A; })
        .addStaticProperty("KEY_B", [] () -> int { return D_KEY_B; })
        .addStaticProperty("KEY_C", [] () -> int { return D_KEY_C; })
        .addStaticProperty("KEY_D", [] () -> int { return D_KEY_D; })
        .addStaticProperty("KEY_E", [] () -> int { return D_KEY_E; })
        .addStaticProperty("KEY_F", [] () -> int { return D_KEY_F; })
        .addStaticProperty("KEY_G", [] () -> int { return D_KEY_G; })
        .addStaticProperty("KEY_H", [] () -> int { return D_KEY_H; })
        .addStaticProperty("KEY_I", [] () -> int { return D_KEY_I; })
        .addStaticProperty("KEY_J", [] () -> int { return D_KEY_J; })
        .addStaticProperty("KEY_K", [] () -> int { return D_KEY_K; })
        .addStaticProperty("KEY_L", [] () -> int { return D_KEY_L; })
        .addStaticProperty("KEY_M", [] () -> int { return D_KEY_M; })
        .addStaticProperty("KEY_N", [] () -> int { return D_KEY_N; })
        .addStaticProperty("KEY_O", [] () -> int { return D_KEY_O; })
        .addStaticProperty("KEY_P", [] () -> int { return D_KEY_P; })
        .addStaticProperty("KEY_Q", [] () -> int { return D_KEY_Q; })
        .addStaticProperty("KEY_R", [] () -> int { return D_KEY_R; })
        .addStaticProperty("KEY_S", [] () -> int { return D_KEY_S; })
        .addStaticProperty("KEY_T", [] () -> int { return D_KEY_T; })
        .addStaticProperty("KEY_U", [] () -> int { return D_KEY_U; })
        .addStaticProperty("KEY_V", [] () -> int { return D_KEY_V; })
        .addStaticProperty("KEY_W", [] () -> int { return D_KEY_W; })
        .addStaticProperty("KEY_X", [] () -> int { return D_KEY_X; })
        .addStaticProperty("KEY_Y", [] () -> int { return D_KEY_Y; })
        .addStaticProperty("KEY_Z", [] () -> int { return D_KEY_Z; })
        .addStaticProperty("KEY_LEFT_BRACKET", [] () -> int { return D_KEY_LEFT_BRACKET; })
        .addStaticProperty("KEY_BACKSLASH", [] () -> int { return D_KEY_BACKSLASH; })
        .addStaticProperty("KEY_RIGHT_BRACKET", [] () -> int { return D_KEY_RIGHT_BRACKET; })
        .addStaticProperty("KEY_GRAVE_ACCENT", [] () -> int { return D_KEY_GRAVE_ACCENT; })
        .addStaticProperty("KEY_WORLD_1", [] () -> int { return D_KEY_WORLD_1; })
        .addStaticProperty("KEY_WORLD_2", [] () -> int { return D_KEY_WORLD_2; })
        .addStaticProperty("KEY_ESCAPE", [] () -> int { return D_KEY_ESCAPE; })
        .addStaticProperty("KEY_ENTER", [] () -> int { return D_KEY_ENTER; })
        .addStaticProperty("KEY_TAB", [] () -> int { return D_KEY_TAB; })
        .addStaticProperty("KEY_BACKSPACE", [] () -> int { return D_KEY_BACKSPACE; })
        .addStaticProperty("KEY_INSERT", [] () -> int { return D_KEY_INSERT; })
        .addStaticProperty("KEY_DELETE", [] () -> int { return D_KEY_DELETE; })
        .addStaticProperty("KEY_RIGHT", [] () -> int { return D_KEY_RIGHT; })
        .addStaticProperty("KEY_LEFT", [] () -> int { return D_KEY_LEFT; })
        .addStaticProperty("KEY_DOWN", [] () -> int { return D_KEY_DOWN; })
        .addStaticProperty("KEY_UP", [] () -> int { return D_KEY_UP; })
        .addStaticProperty("KEY_PAGE_UP", [] () -> int { return D_KEY_PAGE_UP; })
        .addStaticProperty("KEY_PAGE_DOWN", [] () -> int { return D_KEY_PAGE_DOWN; })
        .addStaticProperty("KEY_HOME", [] () -> int { return D_KEY_HOME; })
        .addStaticProperty("KEY_END", [] () -> int { return D_KEY_END; })
        .addStaticProperty("KEY_CAPS_LOCK", [] () -> int { return D_KEY_CAPS_LOCK; })
        .addStaticProperty("KEY_SCROLL_LOCK", [] () -> int { return D_KEY_SCROLL_LOCK; })
        .addStaticProperty("KEY_NUM_LOCK", [] () -> int { return D_KEY_NUM_LOCK; })
        .addStaticProperty("KEY_PRINT_SCREEN", [] () -> int { return D_KEY_PRINT_SCREEN; })
        .addStaticProperty("KEY_PAUSE", [] () -> int { return D_KEY_PAUSE; })
        .addStaticProperty("KEY_F1", [] () -> int { return D_KEY_F1; })
        .addStaticProperty("KEY_F2", [] () -> int { return D_KEY_F2; })
        .addStaticProperty("KEY_F3", [] () -> int { return D_KEY_F3; })
        .addStaticProperty("KEY_F4", [] () -> int { return D_KEY_F4; })
        .addStaticProperty("KEY_F5", [] () -> int { return D_KEY_F5; })
        .addStaticProperty("KEY_F6", [] () -> int { return D_KEY_F6; })
        .addStaticProperty("KEY_F7", [] () -> int { return D_KEY_F7; })
        .addStaticProperty("KEY_F8", [] () -> int { return D_KEY_F8; })
        .addStaticProperty("KEY_F9", [] () -> int { return D_KEY_F9; })
        .addStaticProperty("KEY_F10", [] () -> int { return D_KEY_F10; })
        .addStaticProperty("KEY_F11", [] () -> int { return D_KEY_F11; })
        .addStaticProperty("KEY_F12", [] () -> int { return D_KEY_F12; })
        .addStaticProperty("KEY_F13", [] () -> int { return D_KEY_F13; })
        .addStaticProperty("KEY_F14", [] () -> int { return D_KEY_F14; })
        .addStaticProperty("KEY_F15", [] () -> int { return D_KEY_F15; })
        .addStaticProperty("KEY_F16", [] () -> int { return D_KEY_F16; })
        .addStaticProperty("KEY_F17", [] () -> int { return D_KEY_F17; })
        .addStaticProperty("KEY_F18", [] () -> int { return D_KEY_F18; })
        .addStaticProperty("KEY_F19", [] () -> int { return D_KEY_F19; })
        .addStaticProperty("KEY_F20", [] () -> int { return D_KEY_F20; })
        .addStaticProperty("KEY_F21", [] () -> int { return D_KEY_F21; })
        .addStaticProperty("KEY_F22", [] () -> int { return D_KEY_F22; })
        .addStaticProperty("KEY_F23", [] () -> int { return D_KEY_F23; })
        .addStaticProperty("KEY_F24", [] () -> int { return D_KEY_F24; })
        .addStaticProperty("KEY_F25", [] () -> int { return D_KEY_F25; })
        .addStaticProperty("KEY_KP_0", [] () -> int { return D_KEY_KP_0; })
        .addStaticProperty("KEY_KP_1", [] () -> int { return D_KEY_KP_1; })
        .addStaticProperty("KEY_KP_2", [] () -> int { return D_KEY_KP_2; })
        .addStaticProperty("KEY_KP_3", [] () -> int { return D_KEY_KP_3; })
        .addStaticProperty("KEY_KP_4", [] () -> int { return D_KEY_KP_4; })
        .addStaticProperty("KEY_KP_5", [] () -> int { return D_KEY_KP_5; })
        .addStaticProperty("KEY_KP_6", [] () -> int { return D_KEY_KP_6; })
        .addStaticProperty("KEY_KP_7", [] () -> int { return D_KEY_KP_7; })
        .addStaticProperty("KEY_KP_8", [] () -> int { return D_KEY_KP_8; })
        .addStaticProperty("KEY_KP_9", [] () -> int { return D_KEY_KP_9; })
        .addStaticProperty("KEY_KP_DECIMAL", [] () -> int { return D_KEY_KP_DECIMAL; })
        .addStaticProperty("KEY_KP_DIVIDE", [] () -> int { return D_KEY_KP_DIVIDE; })
        .addStaticProperty("KEY_KP_MULTIPLY", [] () -> int { return D_KEY_KP_MULTIPLY; })
        .addStaticProperty("KEY_KP_SUBTRACT", [] () -> int { return D_KEY_KP_SUBTRACT; })
        .addStaticProperty("KEY_KP_ADD", [] () -> int { return D_KEY_KP_ADD; })
        .addStaticProperty("KEY_KP_ENTER", [] () -> int { return D_KEY_KP_ENTER; })
        .addStaticProperty("KEY_KP_EQUAL", [] () -> int { return D_KEY_KP_EQUAL; })
        .addStaticProperty("KEY_LEFT_SHIFT", [] () -> int { return D_KEY_LEFT_SHIFT; })
        .addStaticProperty("KEY_LEFT_CONTROL", [] () -> int { return D_KEY_LEFT_CONTROL; })
        .addStaticProperty("KEY_LEFT_ALT", [] () -> int { return D_KEY_LEFT_ALT; })
        .addStaticProperty("KEY_LEFT_SUPER", [] () -> int { return D_KEY_LEFT_SUPER; })
        .addStaticProperty("KEY_RIGHT_SHIFT", [] () -> int { return D_KEY_RIGHT_SHIFT; })
        .addStaticProperty("KEY_RIGHT_CONTROL", [] () -> int { return D_KEY_RIGHT_CONTROL; })
        .addStaticProperty("KEY_RIGHT_ALT", [] () -> int { return D_KEY_RIGHT_ALT; })
        .addStaticProperty("KEY_RIGHT_SUPER", [] () -> int { return D_KEY_RIGHT_SUPER; })
        .addStaticProperty("KEY_MENU", [] () -> int { return D_KEY_MENU; })
        .addStaticProperty("KEY_LAST", [] () -> int { return D_KEY_LAST; })

        .addStaticProperty("MOUSE_BUTTON_1", [] () -> int { return D_MOUSE_BUTTON_1; })
        .addStaticProperty("MOUSE_BUTTON_2", [] () -> int { return D_MOUSE_BUTTON_2; })
        .addStaticProperty("MOUSE_BUTTON_3", [] () -> int { return D_MOUSE_BUTTON_3; })
        .addStaticProperty("MOUSE_BUTTON_4", [] () -> int { return D_MOUSE_BUTTON_4; })
        .addStaticProperty("MOUSE_BUTTON_5", [] () -> int { return D_MOUSE_BUTTON_5; })
        .addStaticProperty("MOUSE_BUTTON_6", [] () -> int { return D_MOUSE_BUTTON_6; })
        .addStaticProperty("MOUSE_BUTTON_7", [] () -> int { return D_MOUSE_BUTTON_7; })
        .addStaticProperty("MOUSE_BUTTON_8", [] () -> int { return D_MOUSE_BUTTON_8; })
        .addStaticProperty("MOUSE_BUTTON_LAST", [] () -> int { return D_MOUSE_BUTTON_LAST; })
        .addStaticProperty("MOUSE_BUTTON_LEFT", [] () -> int { return D_MOUSE_BUTTON_LEFT; })
        .addStaticProperty("MOUSE_BUTTON_RIGHT", [] () -> int { return D_MOUSE_BUTTON_RIGHT; })
        .addStaticProperty("MOUSE_BUTTON_MIDDLE", [] () -> int { return D_MOUSE_BUTTON_MIDDLE; })

        .addStaticProperty("GAMEPAD_BUTTON_A", [] () -> int { return D_GAMEPAD_BUTTON_A; })
        .addStaticProperty("GAMEPAD_BUTTON_B", [] () -> int { return D_GAMEPAD_BUTTON_B; })
        .addStaticProperty("GAMEPAD_BUTTON_X", [] () -> int { return D_GAMEPAD_BUTTON_X; })
        .addStaticProperty("GAMEPAD_BUTTON_Y", [] () -> int { return D_GAMEPAD_BUTTON_Y; })
        .addStaticProperty("GAMEPAD_BUTTON_LEFT_BUMPER", [] () -> int { return D_GAMEPAD_BUTTON_LEFT_BUMPER; })
        .addStaticProperty("GAMEPAD_BUTTON_RIGHT_BUMPER", [] () -> int { return D_GAMEPAD_BUTTON_RIGHT_BUMPER; })
        .addStaticProperty("GAMEPAD_BUTTON_BACK", [] () -> int { return D_GAMEPAD_BUTTON_BACK; })
        .addStaticProperty("GAMEPAD_BUTTON_START", [] () -> int { return D_GAMEPAD_BUTTON_START; })
        .addStaticProperty("GAMEPAD_BUTTON_GUIDE", [] () -> int { return D_GAMEPAD_BUTTON_GUIDE; })
        .addStaticProperty("GAMEPAD_BUTTON_LEFT_THUMB", [] () -> int { return D_GAMEPAD_BUTTON_LEFT_THUMB; })
        .addStaticProperty("GAMEPAD_BUTTON_RIGHT_THUMB", [] () -> int { return D_GAMEPAD_BUTTON_RIGHT_THUMB; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_UP", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_UP; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_RIGHT", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_RIGHT; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_DOWN", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_DOWN; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_LEFT", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_LEFT; })
        .addStaticProperty("GAMEPAD_BUTTON_LAST", [] () -> int { return D_GAMEPAD_BUTTON_LAST; })
        .addStaticProperty("GAMEPAD_BUTTON_CROSS", [] () -> int { return D_GAMEPAD_BUTTON_CROSS; })
        .addStaticProperty("GAMEPAD_BUTTON_CIRCLE", [] () -> int { return D_GAMEPAD_BUTTON_CIRCLE; })
        .addStaticProperty("GAMEPAD_BUTTON_SQUARE", [] () -> int { return D_GAMEPAD_BUTTON_SQUARE; })
        .addStaticProperty("GAMEPAD_BUTTON_TRIANGLE", [] () -> int { return D_GAMEPAD_BUTTON_TRIANGLE; })

        .addStaticProperty("GAMEPAD_AXIS_LEFT_X", [] () -> int { return D_GAMEPAD_AXIS_LEFT_X; })
        .addStaticProperty("GAMEPAD_AXIS_LEFT_Y", [] () -> int { return D_GAMEPAD_AXIS_LEFT_Y; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_X", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_X; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_Y", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_Y; })
        .addStaticProperty("GAMEPAD_AXIS_LEFT_TRIGGER", [] () -> int { return D_GAMEPAD_AXIS_LEFT_TRIGGER; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_TRIGGER", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_TRIGGER; })
        .addStaticProperty("GAMEPAD_AXIS_LAST", [] () -> int { return D_GAMEPAD_AXIS_LAST; })

        // Touch.phase (getTouch) and getPointerLockState()
        .addStaticProperty("TOUCH_BEGAN", [] () -> int { return static_cast<int>(TouchPhase::BEGAN); })
        .addStaticProperty("TOUCH_MOVED", [] () -> int { return static_cast<int>(TouchPhase::MOVED); })
        .addStaticProperty("TOUCH_STATIONARY", [] () -> int { return static_cast<int>(TouchPhase::STATIONARY); })
        .addStaticProperty("TOUCH_ENDED", [] () -> int { return static_cast<int>(TouchPhase::ENDED); })
        .addStaticProperty("TOUCH_CANCELED", [] () -> int { return static_cast<int>(TouchPhase::CANCELED); })
        .addStaticProperty("POINTER_UNLOCKED", [] () -> int { return static_cast<int>(PointerLockState::UNLOCKED); })
        .addStaticProperty("POINTER_PENDING", [] () -> int { return static_cast<int>(PointerLockState::PENDING); })
        .addStaticProperty("POINTER_LOCKED", [] () -> int { return static_cast<int>(PointerLockState::LOCKED); })

        // levels, and the edges of the frame (of the fixed step inside onFixedUpdate). key: an
        // Input.KEY_* code or a control name ("space", "a", "left_shift", "mouse_left",
        // "gamepad_a", Unity's "left ctrl", "joystick button 0"...)
        .addStaticFunction("getKey",
            +[](int key) { return Input::getKey(key); },
            +[](const std::string& key) -> bool { return luaControlHeld(key); })
        .addStaticFunction("getKeyDown",
            +[](int key) { return Input::getKeyDown(key); },
            +[](const std::string& key) -> bool { return luaControlEdge(key, true); })
        .addStaticFunction("getKeyUp",
            +[](int key) { return Input::getKeyUp(key); },
            +[](const std::string& key) -> bool { return luaControlEdge(key, false); })
        // button: 0 left, 1 right, 2 middle (Input.MOUSE_BUTTON_*)
        .addStaticFunction("getMouseButton", &Input::getMouseButton)
        .addStaticFunction("getMouseButtonDown", &Input::getMouseButtonDown)
        .addStaticFunction("getMouseButtonUp", &Input::getMouseButtonUp)
        // id -1: any gamepad; button: Input.GAMEPAD_BUTTON_* or a name ("gamepad_a", "a", "dpad_up")
        .addStaticFunction("getGamepadButtonDown",
            +[](int id, int button) { return Input::getGamepadButtonDown(id, button); },
            +[](int id, const std::string& button) { return Input::getGamepadButtonDown(id, luaGamepadButton(button)); })
        .addStaticFunction("getGamepadButtonUp",
            +[](int id, int button) { return Input::getGamepadButtonUp(id, button); },
            +[](int id, const std::string& button) { return Input::getGamepadButtonUp(id, luaGamepadButton(button)); })
        // a key, mouse button or gamepad button pressed this frame: a property (Unity) that may
        // be called (Input.anyKeyDown())
        .addStaticProperty("anyKeyDown", +[]() { return Input::anyKeyDown(); })
        // one held now
        .addStaticProperty("anyKey", +[]() { return Input::isAnyKeyPressed(); })
        // UTF-8 text typed this frame (backspace "\b", return "\n")
        .addStaticProperty("inputString", +[]() { return Input::getInputString(); })
        // the first control pressed this frame by name ("space", "mouse_left", "gamepad_a", "left_x+"), nil when none
        .addStaticFunction("getPressedControl", +[]() -> std::optional<std::string> {
            const std::string name = Input::getPressedControl();
            if (name.empty()) return std::nullopt;
            return name;
        })
        .addStaticFunction("getMouseDelta", &Input::getMouseDelta)
        .addStaticFunction("getMouseScrollDelta", &Input::getMouseScrollDelta)
        // the same as properties (Unity): canvas px this frame, y down; wheel steps (y up)
        .addStaticProperty("mouseDelta", +[]() { return Input::getMouseDelta(); })
        .addStaticProperty("mouseScrollDelta", +[]() { return Input::getMouseScrollDelta(); })
        .addStaticFunction("getTouchCount", &Input::getTouchCount)
        .addStaticProperty("touchCount", +[]() { return Input::getTouchCount(); })
        // index 0-based; nil when out of range
        .addStaticFunction("getTouch", +[](int index) -> std::optional<Touch> {
            if (index < 0 || (size_t)index >= Input::getTouchCount()) return std::nullopt;
            return Input::getTouch((size_t)index);
        })
        // the frame's touches (Unity's Input.touches)
        .addStaticProperty("touches", +[]() { return Input::getFrameTouches(); })
        // the touch of that finger this frame, nil when none
        .addStaticFunction("getTouchById", +[](int fingerId) -> std::optional<Touch> {
            Touch touch;
            if (!Input::findTouchById(fingerId, touch)) return std::nullopt;
            return touch;
        })
        .addStaticProperty("multiTouchEnabled", &Input::isMultiTouchEnabled, &Input::setMultiTouchEnabled)
        // System.isTouchDevice()
        .addStaticProperty("touchSupported", +[]() { return System::instance().isTouchDevice(); })
        .addStaticFunction("getTouchDown", &Input::getTouchDown)
        .addStaticFunction("getTouchUp", &Input::getTouchUp)
        .addStaticFunction("getPointerLockState", &Input::getPointerLockState)

        // the Input Manager's actions (an action's flat name, the "Input Manager" map's first,
        // or "Map/Action"): smoothed, -1..1 (0..1 for a button; mouse movement unclamped)
        .addStaticFunction("getAxis", +[](const std::string& name) { return InputActions::getAxis(name); })
        // without smoothing: -1, 0 or 1 for keys
        .addStaticFunction("getAxisRaw", +[](const std::string& name) { return InputActions::getAxisRaw(name); })
        .addStaticFunction("getButton", +[](const std::string& name) { return InputActions::getButton(name); })
        .addStaticFunction("getButtonDown", +[](const std::string& name) { return InputActions::getButtonDown(name); })
        .addStaticFunction("getButtonUp", +[](const std::string& name) { return InputActions::getButtonUp(name); })
        // (getAxis(xName), getAxis(yName)) with the length clamped to 1 (Godot's get_vector)
        .addStaticFunction("getVector", +[](const std::string& xName, const std::string& yName) { return InputActions::getVector(xName, yName); })
        .addStaticFunction("hasAction", +[](const std::string& name) { return InputActions::hasAction(name); })
        .addStaticFunction("getActionNames", +[]() { return InputActions::getActionNames(); })
        // a disabled action reads 0 / false
        .addStaticFunction("setActionEnabled", +[](const std::string& name, bool enabled) { InputActions::setActionEnabled(name, enabled); })
        .addStaticFunction("isActionEnabled", +[](const std::string& name) { return InputActions::isActionEnabled(name); })
        .addStaticFunction("simulateButton", +[](const std::string& name, bool pressed, luabridge::LuaRef source) {
            InputActions::setSourceValue(luaSourceOwner(source), name, pressed ? 1.0f : 0.0f, InputSourceKind::Script);
        })
        .addStaticFunction("simulateAxis", +[](const std::string& name, float value, luabridge::LuaRef source) {
            InputActions::setSourceValue(luaSourceOwner(source), name, value, InputSourceKind::Script);
        })
        .addStaticFunction("addAxisDelta", +[](const std::string& name, float delta, luabridge::LuaRef source) {
            InputActions::addSourceDelta(luaSourceOwner(source), name, delta, InputSourceKind::Script);
        })
        .addStaticFunction("clearSimulated", +[](luabridge::LuaRef source) {
            if (source.isNil()) InputActions::clearSources(InputSourceKind::Script);
            else InputActions::clearSource(luaSourceOwner(source));
        })
        // ui-multitouch D15: the pointer (-1 the mouse, a finger's fingerId; none: the mouse or
        // any finger) is over component UI that takes events or HTML content, or either holds it
        .addStaticFunction("isPointerOverUI", +[](luabridge::LuaRef pointerId) {
            if (pointerId.isNil()) return Engine::isPointerOverUI(INT_MIN);
            if (!pointerId.isNumber()) return false;
            return Engine::isPointerOverUI(pointerId.unsafe_cast<int>());
        })
        .addStaticFunction("getBindings", &luaInputGetBindings)
        .addStaticFunction("setBindings", &luaInputSetBindings)
        // undoes setBindings / importBindings of the action (without a name: of every action)
        .addStaticFunction("resetBindings",
            +[]() { InputActions::resetBindings(""); },
            +[](const std::string& name) { InputActions::resetBindings(name); })
        // the replacements as JSON text: keep it with UserSettings, give it back to importBindings
        .addStaticFunction("exportBindings", +[]() { return InputActions::exportOverrides(); })
        .addStaticFunction("importBindings", &luaInputImportBindings)

        .addStaticFunction("isKeyPressed", &Input::isKeyPressed)
        .addStaticFunction("isMousePressed", &Input::isMousePressed)
        .addStaticFunction("isTouch", &Input::isTouch)
        .addStaticFunction("isMouseEntered", &Input::isMouseEntered)
        .addStaticFunction("getMousePosition", &Input::getMousePosition)
        .addStaticFunction("getMouseScroll", &Input::getMouseScroll)
        .addStaticFunction("getTouchPosition", &Input::getTouchPosition)
        .addStaticFunction("getTouches", &Input::getTouches)
        .addStaticFunction("numTouches", &Input::numTouches)
        .addStaticFunction("isGamepadConnected", &Input::isGamepadConnected)
        .addStaticFunction("getGamepadName", &Input::getGamepadName)
        .addStaticFunction("isGamepadButtonPressed", &Input::isGamepadButtonPressed)
        .addStaticFunction("getGamepadAxis", &Input::getGamepadAxis)
        .addStaticFunction("numGamepads", &Input::numGamepads)
        .addStaticFunction("getGamepadId", &Input::getGamepadId)
        .addStaticFunction("getModifiers", &Input::getModifiers)
        .addStaticFunction("findTouchIndex", &Input::findTouchIndex)
        // Unity's Input.deviceOrientation (platform services)
        .addStaticProperty("deviceOrientation", &Input::getDeviceOrientation)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<System>("System")
        .addStaticFunction("getScreenWidth", [] () { return System::instance().getScreenWidth(); })
        .addStaticFunction("getScreenHeight", [] () { return System::instance().getScreenHeight(); })
        .addStaticFunction("showVirtualKeyboard", [] (std::wstring text) { System::instance().showVirtualKeyboard(text); })
        .addStaticFunction("hideVirtualKeyboard", [] () { System::instance().hideVirtualKeyboard(); })
        .addStaticFunction("setClipboardText", [] (const std::string& text) { System::instance().setClipboardText(text); })
        .addStaticFunction("getClipboardText", [] () { return System::instance().getClipboardText(); })
        .addStaticFunction("isTouchDevice", [] () { return System::instance().isTouchDevice(); })
        .addStaticFunction("isFullscreen", [] () { return System::instance().isFullscreen(); })
        .addStaticFunction("requestFullscreen", [] () { System::instance().requestFullscreen(); })
        .addStaticFunction("exitFullscreen", [] () { System::instance().exitFullscreen(); })
        .addStaticFunction("isWindowMaximized", [] () { return System::instance().isWindowMaximized(); })
        .addStaticFunction("maximizeWindow", [] () { System::instance().maximizeWindow(); })
        .addStaticFunction("restoreWindow", [] () { System::instance().restoreWindow(); })
        .addStaticFunction("setWindowSize", [] (int width, int height) { System::instance().setWindowSize(width, height); })
        .addStaticFunction("isWindowResizable", [] () { return System::instance().isWindowResizable(); })
        .addStaticFunction("setWindowResizable", [] (bool resizable) { System::instance().setWindowResizable(resizable); })
        .addStaticFunction("setWindowTitle", [] (const std::string& title) { System::instance().setWindowTitle(title); })
        .addStaticFunction("quit", [] () { System::instance().quit(); })
        // the game's command line arguments: what follows "--" (a prebuilt player), or what the
        // engine's options do not use (a Compile build)
        .addStaticFunction("getArgs", [] (lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            int index = 1;
            for (const std::string& arg : player::RunControl::options().scriptArgs)
                table[index++] = arg;
            return table;
        })
        .addStaticProperty("args", [] (lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            int index = 1;
            for (const std::string& arg : player::RunControl::options().scriptArgs)
                table[index++] = arg;
            return table;
        })
        .addStaticFunction("getDirSeparator", [] () { return System::instance().getDirSeparator(); })
        .addStaticFunction("getAssetPath", [] () { return System::instance().getAssetPath(); })
        .addStaticFunction("getUserDataPath", [] () { return System::instance().getUserDataPath(); })
        .addStaticFunction("getLuaPath", [] () { return System::instance().getLuaPath(); })
        .addStaticFunction("getShaderPath", [] () { return System::instance().getShaderPath(); })
        // The web build: writes the user data files (data://) to IndexedDB now instead of in
        // half a second (File.writeText and Json.writeFile ask for that write themselves; Lua io
        // writes need this call). Elsewhere files are on disk already: true.
        .addStaticFunction("syncFileSystem", [] () { return System::instance().flushFileSystem(); })
        // The web build: the saved files are restored from IndexedDB (the first scene waits for
        // it, up to 3 s). Elsewhere always true.
        .addStaticFunction("isUserDataLoaded", [] () { return System::instance().isUserDataLoaded(); })

        // platform services
        .addStaticFunction("openURL", [] (const std::string& url) { return System::instance().openURL(url); })
        .addStaticFunction("getSystemLanguage", [] () { return System::instance().getSystemLanguage(); })
        .addStaticFunction("getLanguageCode", [] () { return System::instance().getLanguageCode(); })
        .addStaticFunction("getRegionCode", [] () { return System::instance().getRegionCode(); })
        .addStaticFunction("getLocale", [] () { return System::instance().getLocale(); })
        .addStaticFunction("getPreferredLanguages", [] () { return System::instance().getPreferredLocales(); })
        .addStaticFunction("getAppVersion", [] () { return System::instance().getAppVersion(); })
        .addStaticFunction("getAppBuild", [] () { return System::instance().getAppBuild(); })
        .addStaticFunction("getAppIdentifier", [] () { return System::instance().getAppIdentifier(); })
        .addStaticFunction("getAppName", [] () { return System::instance().getAppName(); })
        .addStaticFunction("getOperatingSystem", [] () { return System::instance().getOperatingSystem(); })
        .addStaticFunction("getOperatingSystemVersion", [] () { return System::instance().getOperatingSystemVersion(); })
        .addStaticFunction("getAndroidApiLevel", [] () { return System::instance().getAndroidApiLevel(); })
        .addStaticFunction("getDeviceModel", [] () { return System::instance().getDeviceModel(); })
        .addStaticFunction("getDeviceType", [] () { return System::instance().getDeviceType(); })
        .addStaticFunction("getSystemMemorySize", [] () { return System::instance().getSystemMemorySize(); })
        .addStaticFunction("getProcessorCount", [] () { return System::instance().getProcessorCount(); })
        .addStaticFunction("isEditor", [] () { return System::instance().isEditor(); })
        .addStaticFunction("getInternetReachability", [] () { return System::instance().getInternetReachability(); })
        // Handheld.Vibrate: 400 ms by default (Unity's length); iOS ignores the length
        .addStaticFunction("vibrate",
            [] () { return System::instance().vibrate(400); },
            [] (int milliseconds) { return System::instance().vibrate(milliseconds); })
        .addStaticFunction("hapticFeedback", [] (HapticFeedback type) { return System::instance().hapticFeedback(type); })

        //UserSettings not need here

        .addStaticFunction("initializeCrazyGamesSDK", [] () { return System::instance().initializeCrazyGamesSDK(); })
        .addStaticFunction("showCrazyGamesAd", [] (std::string type) { return System::instance().showCrazyGamesAd(type); })
        .addStaticFunction("happytimeCrazyGames", [] () { return System::instance().happytimeCrazyGames(); })
        .addStaticFunction("gameplayStartCrazyGames", [] () { return System::instance().gameplayStartCrazyGames(); })
        .addStaticFunction("gameplayStopCrazyGames", [] () { return System::instance().gameplayStopCrazyGames(); })
        .addStaticFunction("loadingStartCrazyGames", [] () { return System::instance().loadingStartCrazyGames(); })
        .addStaticFunction("loadingStopCrazyGames", [] () { return System::instance().loadingStopCrazyGames(); })
        .endClass();

    // The device the game runs on (docs/device-tiers.md): its GPU, the Device Tier it gets, the
    // thermal state and memory warnings
    luabridge::getGlobalNamespace(L)
        .beginClass<Device>("Device")
        // the Device Tier in effect (DeviceTier): setTier's, else the detected one
        .addStaticProperty("tier", &Device::getTier)
        // the GPU's name, as detection read it ("Adreno (TM) 650", "Apple A15 GPU")
        .addStaticProperty("gpuName", &Device::getGpuName)
        // why the device got its tier: "Adreno (TM) 650, rule name (adreno 650), tier Medium"
        .addStaticProperty("tierReason", [] () { return Device::getDetection().reason; })
        // whether a tier's overrides apply (false in the editor until a tier is simulated or set)
        .addStaticProperty("tierApplied", &Device::isTierApplied)
        // the platform's thermal state (ThermalState); Nominal on desktops and the web
        .addStaticProperty("thermalState", &Device::getThermalState)
        // a graphics menu's choice: this tier's overrides for the rest of the run
        .addStaticFunction("setTier", &Device::setTier)
        // back to the detected tier
        .addStaticFunction("resetTier", &Device::resetTier)
        // the thermal state changed: function(state) with the new ThermalState
        .addStaticProperty("onThermalStateChanged", [] () { return &Device::onThermalStateChanged; }, [] (lua_State* L) { Device::onThermalStateChanged = L; })
        // a low-memory warning was handled: the caches were cleared and textures keep a mip off
        .addStaticProperty("onLowMemory", [] () { return &Device::onLowMemory; }, [] (lua_State* L) { Device::onLowMemory = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Screen>("Screen")
        .addStaticProperty("orientation", &Screen::getOrientation, &Screen::setOrientation)
        .addStaticProperty("autorotateToPortrait",
            [] () { return Screen::getAutorotate(ScreenOrientation::Portrait); },
            [] (bool allowed) { Screen::setAutorotate(ScreenOrientation::Portrait, allowed); })
        .addStaticProperty("autorotateToPortraitUpsideDown",
            [] () { return Screen::getAutorotate(ScreenOrientation::PortraitUpsideDown); },
            [] (bool allowed) { Screen::setAutorotate(ScreenOrientation::PortraitUpsideDown, allowed); })
        .addStaticProperty("autorotateToLandscapeLeft",
            [] () { return Screen::getAutorotate(ScreenOrientation::LandscapeLeft); },
            [] (bool allowed) { Screen::setAutorotate(ScreenOrientation::LandscapeLeft, allowed); })
        .addStaticProperty("autorotateToLandscapeRight",
            [] () { return Screen::getAutorotate(ScreenOrientation::LandscapeRight); },
            [] (bool allowed) { Screen::setAutorotate(ScreenOrientation::LandscapeRight, allowed); })
        .addStaticProperty("sleepTimeout", &Screen::getSleepTimeout, &Screen::setSleepTimeout)
        .addStaticProperty("dpi", &Screen::getDpi)
        .addStaticProperty("refreshRate", &Screen::getRefreshRate)
        .addStaticProperty("safeArea", &Screen::getSafeArea)
        .addStaticProperty("safeAreaInsets", &Screen::getSafeAreaInsets)
        .addStaticProperty("deviceSafeArea", &Screen::getDeviceSafeArea)
        .addStaticProperty("deviceSafeAreaInsets", &Screen::getDeviceSafeAreaInsets)
        .addStaticProperty("safeAreaPixels", &Screen::getSafeAreaPixels)
        .addStaticProperty("cutouts", &Screen::getCutouts)
        .addStaticProperty("extraInsets", &Screen::getExtraInsets)
        .addStaticFunction("setExtraInsets", &luaScreenSetExtraInsets)
        .addStaticFunction("clearExtraInsets", &Screen::clearExtraInsets)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
