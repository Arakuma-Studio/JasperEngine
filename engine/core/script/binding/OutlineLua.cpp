// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The Outline component in Lua (docs/specs/S94-rendering-anim-gaps.md F22): the OutlineMode and
// OutlineWidthUnits tables and the Outline handle.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "component/OutlineComponent.h"
#include "object/Outline.h"

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace luabridge {
    template<> struct Stack<OutlineMode> : EnumWrapper<OutlineMode>{};
    template<> struct Stack<OutlineWidthUnits> : EnumWrapper<OutlineWidthUnits>{};
}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerOutlineClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        .beginNamespace("OutlineMode")
        .addVariable("OUTLINE_ALL", OutlineMode::OUTLINE_ALL)
        .addVariable("OUTLINE_VISIBLE", OutlineMode::OUTLINE_VISIBLE)
        .addVariable("OUTLINE_HIDDEN", OutlineMode::OUTLINE_HIDDEN)
        .addVariable("OUTLINE_AND_SILHOUETTE", OutlineMode::OUTLINE_AND_SILHOUETTE)
        .addVariable("SILHOUETTE_ONLY", OutlineMode::SILHOUETTE_ONLY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("OutlineWidthUnits")
        .addVariable("PIXELS", OutlineWidthUnits::PIXELS)
        .addVariable("WORLD", OutlineWidthUnits::WORLD)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Outline, EntityHandle>("Outline")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &Outline::isEnabled, &Outline::setEnabled)
        .addProperty("mode", &Outline::getMode, &Outline::setMode)
        .addProperty("color", &Outline::getColor, &Outline::setColor)
        .addProperty("width", &Outline::getWidth, &Outline::setWidth)
        .addProperty("widthUnits", &Outline::getWidthUnits, &Outline::setWidthUnits)
        .addProperty("includeChildren", &Outline::isIncludeChildren, &Outline::setIncludeChildren)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
