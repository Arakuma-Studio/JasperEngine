// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The <video> API of UIElement (phase 6, p6-video; docs/html-ui/phase-6-video.md §3.9), with
// HTMLMediaElement's names: properties (currentTime, duration, paused, volume, ...), methods
// (load, canPlayType, fastSeek; play and pause are the Lottie control's, which forward to a
// video) and the constants on the class table (UIElement.HAVE_ENOUGH_DATA). Re-opens the UIElement
// class as HtmlUIVectorLua.cpp does; a dead handle raises kHtmlUIDeadElementMessage, another
// element logs an error and reads as the neutral value.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "object/ui/UIElement.h"
#include "object/ui/HtmlUIScriptInternal.h"

// the video module (docs/specs/S34-player-size.md R1): the Lua API description reads this condition
#if JASPERENGINE_VIDEO

using namespace jasperengine;

namespace {

    void checkMediaElement(const UIElement* self, lua_State* L){
        if (!self || !self->isValid())
            luaL_error(L, "%s", kHtmlUIDeadElementMessage);
    }

    template <int Value>
    int constant(){
        return Value;
    }

}

void LuaBinding::registerHtmlUIMediaClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginClass<UIElement>("UIElement")
        .addStaticProperty("HAVE_NOTHING", &constant<UIElement::HAVE_NOTHING>)
        .addStaticProperty("HAVE_METADATA", &constant<UIElement::HAVE_METADATA>)
        .addStaticProperty("HAVE_CURRENT_DATA", &constant<UIElement::HAVE_CURRENT_DATA>)
        .addStaticProperty("HAVE_FUTURE_DATA", &constant<UIElement::HAVE_FUTURE_DATA>)
        .addStaticProperty("HAVE_ENOUGH_DATA", &constant<UIElement::HAVE_ENOUGH_DATA>)
        .addStaticProperty("NETWORK_EMPTY", &constant<UIElement::NETWORK_EMPTY>)
        .addStaticProperty("NETWORK_IDLE", &constant<UIElement::NETWORK_IDLE>)
        .addStaticProperty("NETWORK_LOADING", &constant<UIElement::NETWORK_LOADING>)
        .addStaticProperty("NETWORK_NO_SOURCE", &constant<UIElement::NETWORK_NO_SOURCE>)
        .addStaticProperty("MEDIA_ERR_ABORTED", &constant<UIElement::MEDIA_ERR_ABORTED>)
        .addStaticProperty("MEDIA_ERR_NETWORK", &constant<UIElement::MEDIA_ERR_NETWORK>)
        .addStaticProperty("MEDIA_ERR_DECODE", &constant<UIElement::MEDIA_ERR_DECODE>)
        .addStaticProperty("MEDIA_ERR_SRC_NOT_SUPPORTED", &constant<UIElement::MEDIA_ERR_SRC_NOT_SUPPORTED>)
        .addFunction("load", [](UIElement* self, lua_State* L) { checkMediaElement(self, L); self->load(); })
        .addFunction("canPlayType", [](UIElement* self, const std::string& type, lua_State* L) -> std::string {
            checkMediaElement(self, L);
            return self->canPlayType(type);
        })
        .addFunction("fastSeek", [](UIElement* self, double seconds, lua_State* L) { checkMediaElement(self, L); self->fastSeek(seconds); })
        .addProperty("currentTime",
            [](const UIElement* self, lua_State* L) -> double { checkMediaElement(self, L); return self->getCurrentTime(); },
            [](UIElement* self, double seconds, lua_State* L) { checkMediaElement(self, L); self->setCurrentTime(seconds); })
        .addProperty("duration", [](const UIElement* self, lua_State* L) -> double { checkMediaElement(self, L); return self->getMediaDuration(); })
        .addProperty("paused", [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->isPaused(); })
        .addProperty("ended", [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->isEnded(); })
        .addProperty("seeking", [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->isSeeking(); })
        .addProperty("loop",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->getLoop(); },
            [](UIElement* self, bool loop, lua_State* L) { checkMediaElement(self, L); self->setLoop(loop); })
        .addProperty("autoplay",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->getAutoplay(); },
            [](UIElement* self, bool autoplay, lua_State* L) { checkMediaElement(self, L); self->setAutoplay(autoplay); })
        .addProperty("controls",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->getControls(); },
            [](UIElement* self, bool controls, lua_State* L) { checkMediaElement(self, L); self->setControls(controls); })
        .addProperty("muted",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->isMuted(); },
            [](UIElement* self, bool muted, lua_State* L) { checkMediaElement(self, L); self->setMuted(muted); })
        .addProperty("defaultMuted",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->getDefaultMuted(); },
            [](UIElement* self, bool muted, lua_State* L) { checkMediaElement(self, L); self->setDefaultMuted(muted); })
        .addProperty("volume",
            [](const UIElement* self, lua_State* L) -> double { checkMediaElement(self, L); return self->getVolume(); },
            [](UIElement* self, double volume, lua_State* L) { checkMediaElement(self, L); self->setVolume((float)volume); })
        .addProperty("playbackRate",
            [](const UIElement* self, lua_State* L) -> double { checkMediaElement(self, L); return self->getPlaybackRate(); },
            [](UIElement* self, double rate, lua_State* L) { checkMediaElement(self, L); self->setPlaybackRate(rate); })
        .addProperty("defaultPlaybackRate",
            [](const UIElement* self, lua_State* L) -> double { checkMediaElement(self, L); return self->getDefaultPlaybackRate(); },
            [](UIElement* self, double rate, lua_State* L) { checkMediaElement(self, L); self->setDefaultPlaybackRate(rate); })
        .addProperty("preservesPitch",
            [](const UIElement* self, lua_State* L) -> bool { checkMediaElement(self, L); return self->getPreservesPitch(); },
            [](UIElement* self, bool preserve, lua_State* L) { checkMediaElement(self, L); self->setPreservesPitch(preserve); })
        .addProperty("readyState", [](const UIElement* self, lua_State* L) -> int { checkMediaElement(self, L); return self->getReadyState(); })
        .addProperty("networkState", [](const UIElement* self, lua_State* L) -> int { checkMediaElement(self, L); return self->getNetworkState(); })
        .addProperty("videoWidth", [](const UIElement* self, lua_State* L) -> int { checkMediaElement(self, L); return self->getVideoWidth(); })
        .addProperty("videoHeight", [](const UIElement* self, lua_State* L) -> int { checkMediaElement(self, L); return self->getVideoHeight(); })
        .addProperty("currentSrc", [](const UIElement* self, lua_State* L) -> std::string { checkMediaElement(self, L); return self->getCurrentSrc(); })
        // what decodes it: "webm" (AV1/Opus in software), "ffmpeg", "mediacodec", "avfoundation", "mediafoundation", "browser"
        .addProperty("videoBackend", [](const UIElement* self, lua_State* L) -> std::string { checkMediaElement(self, L); return self->getVideoBackend(); })
        .addProperty("poster",
            [](const UIElement* self, lua_State* L) -> std::string { checkMediaElement(self, L); return self->getPoster(); },
            [](UIElement* self, const std::string& poster, lua_State* L) { checkMediaElement(self, L); self->setPoster(poster); })
        .addProperty("preload",
            [](const UIElement* self, lua_State* L) -> std::string { checkMediaElement(self, L); return self->getPreload(); },
            [](UIElement* self, const std::string& preload, lua_State* L) { checkMediaElement(self, L); self->setPreload(preload); })
        // nil, or { code = 1..4, message = "..." } (HTML's MediaError)
        .addProperty("error", [](const UIElement* self, lua_State* L) -> luabridge::LuaRef {
            checkMediaElement(self, L);
            const std::optional<UIMediaError> error = self->getMediaError();
            if (!error)
                return luabridge::LuaRef(L);
            luabridge::LuaRef table = luabridge::newTable(L);
            table["code"] = error->code;
            table["message"] = error->message;
            return table;
        })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_VIDEO
