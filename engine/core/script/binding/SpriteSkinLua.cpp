// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// 2D skeletal sprites in Lua (docs/specs/S94-rendering-anim-gaps.md F49): SpriteSkin, the handle
// of a GameObject's Sprite Skin. Move, rotate or animate the bone GameObjects to bend the Sprite.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "object/SpriteSkin.h"

using namespace jasperengine;

void LuaBinding::registerSpriteSkinClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        .deriveClass<SpriteSkin, EntityHandle>("SpriteSkin")
        // SpriteSkin(scene, entity): the GameObject's Sprite Skin, added when it has none
        .addConstructor <void (*) (Scene*, Entity)> ()
        // off: the Sprite draws its frame's quad
        .addProperty("enabled", &SpriteSkin::isEnabled, &SpriteSkin::setEnabled)
        // the skeleton's top GameObject (Unity's Root Bone)
        .addProperty("rootBone", &SpriteSkin::getRootBone, &SpriteSkin::setRootBone)
        // the bone GameObjects (entities), in the order of the frame skin's bones
        .addProperty("bones", &SpriteSkin::getBones, &SpriteSkin::setBones)
        // how many bones the shown frame's skin has (0: no skin, the Sprite draws its quad)
        .addProperty("skinBoneCount", &SpriteSkin::getSkinBoneCount)
        // the Sprite draws through its bones now
        .addProperty("skinned", &SpriteSkin::isSkinned)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
