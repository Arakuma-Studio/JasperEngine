// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The <lottie> control of UIElement (p1-vector). Re-opens the UIElement class that
// registerHtmlUIClasses (p1-lua) binds and adds the Lottie methods; LuaBridge allows adding
// members to a registered class. Like every UIElement method, a dead handle raises
// "UIElement: element no longer exists ..." (the error is raised before any C++ object with a
// destructor is alive: Lua is compiled as C and luaL_error longjmps); another element logs an
// error and does nothing.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "object/ui/UIElement.h"

using namespace jasperengine;

namespace {

    void checkElement(const UIElement* self, lua_State* L){
        if (!self || !self->isValid())
            luaL_error(L, "UIElement: element no longer exists (its document was unloaded or it was removed)");
    }

}

void LuaBinding::registerHtmlUIVectorClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginClass<UIElement>("UIElement")
        .addFunction("play", [](UIElement* self, lua_State* L) { checkElement(self, L); self->play(); })
        .addFunction("pause", [](UIElement* self, lua_State* L) { checkElement(self, L); self->pause(); })
        .addFunction("stop", [](UIElement* self, lua_State* L) { checkElement(self, L); self->stop(); })
        .addFunction("seek", [](UIElement* self, float seconds, lua_State* L) { checkElement(self, L); self->seek(seconds); })
        .addFunction("setFrame", [](UIElement* self, float frame, lua_State* L) { checkElement(self, L); self->setFrame(frame); })
        .addFunction("setSpeed", [](UIElement* self, float speed, lua_State* L) { checkElement(self, L); self->setSpeed(speed); })
        .addFunction("setDirection", [](UIElement* self, int direction, lua_State* L) { checkElement(self, L); self->setDirection(direction); })
        .addFunction("setLoop", [](UIElement* self, bool loop, lua_State* L) { checkElement(self, L); self->setLoop(loop); })
        .addFunction("setLoopCount", [](UIElement* self, int count, lua_State* L) { checkElement(self, L); self->setLoopCount(count); })
        .addFunction("setSegment", [](UIElement* self, float startFrame, float endFrame, lua_State* L) { checkElement(self, L); self->setSegment(startFrame, endFrame); })
        .addFunction("setMarker", [](UIElement* self, const char* marker, lua_State* L) { checkElement(self, L); self->setMarker(marker ? marker : ""); })
        .addFunction("isPlaying", [](UIElement* self, lua_State* L) -> bool { checkElement(self, L); return self->isPlaying(); })
        .addFunction("getCurrentFrame", [](UIElement* self, lua_State* L) -> float { checkElement(self, L); return self->getCurrentFrame(); })
        .addFunction("getTotalFrames", [](UIElement* self, lua_State* L) -> float { checkElement(self, L); return self->getTotalFrames(); })
        .addFunction("getDuration", [](UIElement* self, lua_State* L) -> float { checkElement(self, L); return self->getDuration(); })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
