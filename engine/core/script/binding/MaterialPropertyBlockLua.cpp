// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// Property Overrides in Lua (docs/specs/S94-rendering-anim-gaps.md F7, F11): MaterialPropertyBlock,
// Unity's per-renderer values over a shared material (saved with the mesh here), and the Mesh calls
// that set and read it (setPropertyBlock, getPropertyBlock) and a submesh's shader textures
// (setShaderTexture, getShaderTexture).
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions that raise errors
// do their work in helpers and raise only from frames holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "object/Mesh.h"
#include "texture/Material.h"
#include "util/Color.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    using BlockErrorText = char[512];

    ShaderPropertyOverrides* blockOf(lua_State* L) {
        return luabridge::detail::Userdata::get<ShaderPropertyOverrides>(L, 1, false);
    }

    // a Vector4, Vector3 (w = wFill) or Vector2 (z, w = 0) at index
    bool blockVectorArg(lua_State* L, int index, Vector4& out, float wFill) {
        if (luabridge::isInstance<Vector4>(L, index)) {
            if (auto value = luabridge::get<Vector4>(L, index)) { out = *value; return true; }
        } else if (luabridge::isInstance<Vector3>(L, index)) {
            if (auto value = luabridge::get<Vector3>(L, index)) { const Vector3 v = *value; out = Vector4(v.x, v.y, v.z, wFill); return true; }
        } else if (luabridge::isInstance<Vector2>(L, index)) {
            if (auto value = luabridge::get<Vector2>(L, index)) { const Vector2 v = *value; out = Vector4(v.x, v.y, 0.0f, 0.0f); return true; }
        }
        return false;
    }

    enum class BlockValue { FLOAT, VECTOR, COLOR };

    bool applyBlockValue(lua_State* L, BlockValue kind, BlockErrorText& error) {
        static const char* const names[] = {"setFloat", "setVector", "setColor"};
        const char* method = names[(int)kind];
        ShaderPropertyOverrides* block = blockOf(L);
        if (!block || !lua_isstring(L, 2)) {
            std::snprintf(error, sizeof(BlockErrorText), "MaterialPropertyBlock:%s(name, value): name must be a string", method);
            return false;
        }
        const std::string name = lua_tostring(L, 2);
        Vector4 value(0.0f, 0.0f, 0.0f, 0.0f);
        if (kind == BlockValue::FLOAT) {
            if (!lua_isnumber(L, 3)) {
                std::snprintf(error, sizeof(BlockErrorText), "MaterialPropertyBlock:setFloat(\"%s\", value): value must be a number", name.c_str());
                return false;
            }
            value.x = (float)lua_tonumber(L, 3);
        } else if (!blockVectorArg(L, 3, value, kind == BlockValue::COLOR ? 1.0f : 0.0f)) {
            std::snprintf(error, sizeof(BlockErrorText), "MaterialPropertyBlock:%s(\"%s\", value): value must be a Vector4, Vector3 or Vector2",
                          method, name.c_str());
            return false;
        }
        // Unity's SetColor takes sRGB and the shader reads linear (alpha as is)
        if (kind == BlockValue::COLOR) {
            const Vector4 clamped(std::max(0.0f, value.x), std::max(0.0f, value.y), std::max(0.0f, value.z), value.w);
            value = Color::sRGBToLinear(clamped);
        }
        // a name the engine writes every frame is refused (ShaderUniforms::set logs it)
        if (!ShaderUniforms::set(block->values, name, value)) {
            std::snprintf(error, sizeof(BlockErrorText), "MaterialPropertyBlock:%s: '%s' is written by the engine", method, name.c_str());
            return false;
        }
        return true;
    }

    template<BlockValue K>
    int blockSet(lua_State* L) {
        BlockErrorText error;
        if (!applyBlockValue(L, K, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    void setBlockTexture(ShaderPropertyOverrides* block, const std::string& name, const std::string& path) {
        auto found = std::find_if(block->textures.begin(), block->textures.end(), [&name](const auto& entry) { return entry.first == name; });
        if (path.empty()) {
            if (found != block->textures.end()) block->textures.erase(found);
        } else if (found != block->textures.end()) {
            found->second.setPath(path);
        } else {
            block->textures.push_back({name, Texture(path)});
        }
    }

    std::string getBlockTexture(const ShaderPropertyOverrides* block, const std::string& name) {
        const Texture* texture = block->findTexture(name);
        return texture ? texture->getPath() : std::string();
    }

    float getBlockFloat(const ShaderPropertyOverrides* block, const std::string& name) {
        return ShaderUniforms::get(block->values, name).x;
    }

    Vector4 getBlockVector(const ShaderPropertyOverrides* block, const std::string& name) {
        return ShaderUniforms::get(block->values, name);
    }

    Vector4 getBlockColor(const ShaderPropertyOverrides* block, const std::string& name) {
        return Color::linearTosRGB(ShaderUniforms::get(block->values, name));
    }

    bool hasBlockProperty(const ShaderPropertyOverrides* block, const std::string& name) {
        return ShaderUniforms::has(block->values, name) || block->findTexture(name) != nullptr;
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerMaterialPropertyBlockClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        // Unity's MaterialPropertyBlock: values and textures by the shader's member or @texture
        // name, over the material's (Mesh:setPropertyBlock). The built-in Lit takes
        // baseColorFactor, emissiveFactor and baseColorTexture
        .beginClass<ShaderPropertyOverrides>("MaterialPropertyBlock")
        .addConstructor<void (*) (void)>()
        .addFunction("setFloat", &blockSet<BlockValue::FLOAT>)
        .addFunction("setVector", &blockSet<BlockValue::VECTOR>)
        // sRGB in (Unity's SetColor), linear in the shader
        .addFunction("setColor", &blockSet<BlockValue::COLOR>)
        // a project image path; "" removes it
        .addFunction("setTexture", &setBlockTexture)
        .addFunction("getFloat", &getBlockFloat)
        .addFunction("getVector", &getBlockVector)
        .addFunction("getColor", &getBlockColor)
        .addFunction("getTexture", &getBlockTexture)
        .addFunction("hasProperty", &hasBlockProperty)
        .addFunction("clear", &ShaderPropertyOverrides::clear)
        .addFunction("isEmpty", &ShaderPropertyOverrides::empty)
        .endClass();

    // reopened for the calls that take or give a block (the other Mesh members:
    // GameObjectClassesLua.cpp)
    luabridge::getGlobalNamespace(L)
        .beginClass<Mesh>("Mesh")
        // every submesh's overrides, or one submesh's (which win); an empty block removes them
        .addFunction("setPropertyBlock",
            luabridge::overload<const ShaderPropertyOverrides&>(&Mesh::setPropertyBlock),
            luabridge::overload<const ShaderPropertyOverrides&, unsigned int>(&Mesh::setPropertyBlock))
        // a copy: change it, then setPropertyBlock it
        .addFunction("getPropertyBlock",
            luabridge::overload<>(&Mesh::getPropertyBlock),
            luabridge::overload<unsigned int>(&Mesh::getPropertyBlock))
        // a @texture of the submesh's shader (submesh 0 when not given), in its material
        .addFunction("setShaderTexture",
            luabridge::overload<const std::string&, const std::string&>(&Mesh::setShaderTexture),
            luabridge::overload<const std::string&, const std::string&, unsigned int>(&Mesh::setShaderTexture))
        .addFunction("getShaderTexture",
            luabridge::overload<const std::string&>(&Mesh::getShaderTexture),
            luabridge::overload<const std::string&, unsigned int>(&Mesh::getShaderTexture))
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
