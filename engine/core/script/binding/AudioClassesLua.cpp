// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua Audio table, SoundHandle and AudioPlayOn (docs/specs/S37-sound-events.md R13)

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "audio/AudioCapture.h"
#include "component/AudioSourceComponent.h"
#include "object/GameObject.h"
#include "object/sound/Audio.h"
#include "subsystem/AudioSystem.h"

#include <string>

using namespace jasperengine;

// a condition of its own: the Lua API description reads the module from this line alone (S34 R5)
#if JASPERENGINE_SOUND_LAYER
#ifndef DISABLE_LUA_BINDINGS
namespace {

    bool audioOptNumber(lua_State* L, int index, const char* field, float& out) {
        lua_getfield(L, index, field);
        const bool has = lua_isnumber(L, -1) != 0;
        if (has) out = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        return has;
    }

    bool audioOptBool(lua_State* L, int index, const char* field) {
        if (!lua_istable(L, index)) return false;
        lua_getfield(L, index, field);
        const bool value = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return value;
    }

    // Audio.play(eventOrClip [, opts]) -> SoundHandle
    int luaAudioTablePlay(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        PlayOptions options;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "target");
            if (!lua_isnil(L, -1)) {
                auto target = luabridge::get<GameObject*>(L, -1);
                if (!target || !target.value()) return luaL_error(L, "Audio.play: opts.target must be a GameObject");
                options.following(*target.value());
            }
            lua_pop(L, 1);
            lua_getfield(L, 2, "position");
            if (!lua_isnil(L, -1)) {
                auto position = luabridge::get<Vector3>(L, -1);
                if (!position) return luaL_error(L, "Audio.play: opts.position must be a Vector3");
                options.position = position.value();
            }
            lua_pop(L, 1);
            lua_getfield(L, 2, "scene");
            if (!lua_isnil(L, -1)) {
                auto scene = luabridge::get<Scene*>(L, -1);
                if (!scene || !scene.value()) return luaL_error(L, "Audio.play: opts.scene must be a Scene");
                options.scene = scene.value();
            }
            lua_pop(L, 1);
            audioOptNumber(L, 2, "volume", options.volume);
            audioOptNumber(L, 2, "pitch", options.pitch);
            audioOptNumber(L, 2, "delay", options.delay);
            lua_getfield(L, 2, "group");
            if (lua_isstring(L, -1)) options.group = lua_tostring(L, -1);
            lua_pop(L, 1);
            options.unscaled = audioOptBool(L, 2, "unscaled");
        }
        if (!luabridge::push(L, AudioSystem::playFree(name, options, "lua"))) lua_pushnil(L);
        return 1;
    }

    // Audio.stopAll([group]) -> integer
    int luaAudioTableStopAll(lua_State* L) {
        const std::string group = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
        lua_pushinteger(L, AudioSystem::stopFreeAndOneShots(group));
        return 1;
    }

    // a field that is there but not a number is refused as Audio's checks refuse a bad number
    void audioOptTime(lua_State* L, int index, const char* field, float& out) {
        lua_getfield(L, index, field);
        if (lua_isnumber(L, -1)) out = (float)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1)) out = -1.0f;
        lua_pop(L, 1);
    }

    std::string audioOptString(lua_State* L, int index, const char* field, const std::string& fallback) {
        if (!lua_istable(L, index)) return fallback;
        lua_getfield(L, index, field);
        const std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
        lua_pop(L, 1);
        return value;
    }

    // Audio.playMusic(eventOrClip [, opts]) -> boolean
    int luaAudioPlayMusic(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        MusicOptions options;
        if (lua_istable(L, 2)) {
            audioOptTime(L, 2, "crossfade", options.crossfade);
            options.sync = audioOptString(L, 2, "sync", options.sync);
            options.snapshot = audioOptString(L, 2, "snapshot", "");
            audioOptTime(L, 2, "snapshotSeconds", options.snapshotSeconds);
            options.group = audioOptString(L, 2, "group", "");
            audioOptTime(L, 2, "volume", options.volume);
            options.ignoreListenerPause = audioOptBool(L, 2, "ignoreListenerPause");
        }
        lua_pushboolean(L, Audio::playMusic(name, options));
        return 1;
    }

    // Audio.stopMusic([fade [, opts]])
    int luaAudioStopMusic(lua_State* L) {
        float fade = 0.0f;
        if (lua_isnumber(L, 1)) fade = (float)lua_tonumber(L, 1);
        else if (!lua_isnoneornil(L, 1)) fade = -1.0f;
        Audio::stopMusic(fade, audioOptString(L, 2, "sync", "now"));
        return 0;
    }

    // Audio.playStinger(eventOrClip [, sync]) -> SoundHandle
    int luaAudioPlayStinger(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        const std::string sync = lua_isstring(L, 2) ? lua_tostring(L, 2) : "beat";
        if (!luabridge::push(L, Audio::playStinger(name, sync))) lua_pushnil(L);
        return 1;
    }

    // Audio.getMusicPosition() -> {section, bar, beat, beatFraction, bpm, beatsPerBar} | nil
    int luaAudioGetMusicPosition(lua_State* L) {
        const std::optional<MusicPosition> position = Audio::getMusicPosition();
        if (!position) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, position->section);
        lua_setfield(L, -2, "section");
        lua_pushinteger(L, position->bar);
        lua_setfield(L, -2, "bar");
        lua_pushinteger(L, position->beat);
        lua_setfield(L, -2, "beat");
        lua_pushnumber(L, position->beatFraction);
        lua_setfield(L, -2, "beatFraction");
        lua_pushnumber(L, position->bpm);
        lua_setfield(L, -2, "bpm");
        lua_pushinteger(L, position->beatsPerBar);
        lua_setfield(L, -2, "beatsPerBar");
        return 1;
    }

    SoundHandle* luaHandleSelf(lua_State* L) {
        SoundHandle* self = luabridge::detail::Userdata::get<SoundHandle>(L, 1, false);
        if (!self) luaL_error(L, "SoundHandle: call with ':' on a SoundHandle");
        return self;
    }

    // handle:fadeOut(seconds [, opts])
    int luaHandleFadeOut(lua_State* L) {
        SoundHandle* self = luaHandleSelf(L);
        self->fadeOut((float)luaL_checknumber(L, 2), audioOptBool(L, 3, "pause"), audioOptBool(L, 3, "unscaled"));
        return 0;
    }

    // handle:fadeTo(volume, seconds [, opts])
    int luaHandleFadeTo(lua_State* L) {
        SoundHandle* self = luaHandleSelf(L);
        self->fadeTo((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), audioOptBool(L, 4, "unscaled"));
        return 0;
    }

}
#endif
#endif

void LuaBinding::registerAudioClasses(lua_State* L) {
    (void)L;
#if JASPERENGINE_SOUND_LAYER
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        .beginNamespace("AudioPlayOn")
        .addVariable("UIClick", AudioPlayOn::AudioPlayOn_UIClick)
        .addVariable("UIPress", AudioPlayOn::AudioPlayOn_UIPress)
        .addVariable("UIHover", AudioPlayOn::AudioPlayOn_UIHover)
        .addVariable("Collision2D", AudioPlayOn::AudioPlayOn_Collision2D)
        .addVariable("Collision3D", AudioPlayOn::AudioPlayOn_Collision3D)
        .addVariable("Sensor2D", AudioPlayOn::AudioPlayOn_Sensor2D)
        .addVariable("Sensor3D", AudioPlayOn::AudioPlayOn_Sensor3D)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<SoundHandle>("SoundHandle")
        .addConstructor<void (*)()>()
        .addFunction("isValid", &SoundHandle::isValid)
        .addFunction("isPlaying", &SoundHandle::isPlaying)
        .addFunction("stop", &SoundHandle::stop)
        .addFunction("pause", &SoundHandle::pause)
        .addFunction("resume", &SoundHandle::resume)
        .addFunction("fadeOut", &luaHandleFadeOut)
        .addFunction("fadeTo", &luaHandleFadeTo)
        .addProperty("volume", &SoundHandle::getVolume, &SoundHandle::setVolume)
        .addProperty("pitch", &SoundHandle::getPitch, &SoundHandle::setPitch)
        .addProperty("pan", &SoundHandle::getPan, &SoundHandle::setPan)
        .addProperty("position", &SoundHandle::getPosition, &SoundHandle::setPosition)
        .addProperty("time", &SoundHandle::getTime)
        .addProperty("voiceCount", &SoundHandle::getVoiceCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Audio>("Audio")
        .addStaticFunction("play", &luaAudioTablePlay)
        .addStaticFunction("stopAll", &luaAudioTableStopAll)
        .addStaticFunction("mark", +[](const std::string& label) { AudioCapture::mark(label, true); })
        .addStaticFunction("isCapturing", +[]() { return AudioCapture::isActive(); })
        .addStaticFunction("playMusic", &luaAudioPlayMusic)
        .addStaticFunction("stopMusic", &luaAudioStopMusic)
        .addStaticFunction("advanceMusic", &Audio::advanceMusic)
        .addStaticFunction("playStinger", &luaAudioPlayStinger)
        .addStaticFunction("getMusic", &Audio::getMusic)
        .addStaticFunction("getMusicTime", &Audio::getMusicTime)
        .addStaticFunction("getMusicPosition", &luaAudioGetMusicPosition)
        .addStaticProperty("onMusicBeat", [] () { return &Audio::onMusicBeat; }, [] (lua_State* L) { Audio::onMusicBeat = L; })
        .addStaticProperty("onMusicSection", [] () { return &Audio::onMusicSection; }, [] (lua_State* L) { Audio::onMusicSection = L; })
        .endClass();
#else
    (void)L;
#endif
#elif !defined(DISABLE_LUA_BINDINGS)
    // without the sound layer: a script that marks its moments behaves the same (Audio.play is the layer's)
    lua_newtable(L);
    lua_pushcfunction(L, [](lua_State* state) -> int {
        AudioCapture::mark(luaL_checkstring(state, 1), true);
        return 0;
    });
    lua_setfield(L, -2, "mark");
    lua_pushcfunction(L, [](lua_State* state) -> int {
        lua_pushboolean(state, AudioCapture::isActive());
        return 1;
    });
    lua_setfield(L, -2, "isCapturing");
    lua_setglobal(L, "Audio");
#else
    (void)L;
#endif
}
