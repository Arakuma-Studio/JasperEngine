// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fog Profiles and Fog Zones in Lua (docs/specs/S101-vapor.md R36, R43, AC13's engine part):
// FogProfile.load / builtin, get, isSet, clone with set and clear (the shared ones read-only),
// Fog.profile, blendTo on game time, blending, getResolved, the runtime writes and their clears,
// and FogZone's fields.

#include "Engine.h"
#include "Scene.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "object/environment/Fog.h"
#include "object/environment/FogZone.h"
#include "render/FogProfile.h"
#include "render/FogResolve.h"
#include "script/LuaBinding.h"

#include "TempDir.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

namespace lua_fog_profile_test {

using namespace jasperengine;


    // runs a chunk; "" on success, else the error message
    std::string fogProfileRunLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    struct RunningScene{
        Scene scene;
        RunningScene(){
            FogProfileLibrary::clear();
            Engine::setScene(&scene);
        }
        ~RunningScene(){
            Engine::removeScene(&scene);
            FogProfileLibrary::clear();
        }
    };

}

namespace lua_fog_profile_test {

using namespace lua_fog_profile_test;

TEST(LuaFogProfileTest, ProfilesAreReadOnlyAndClonesChange){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    RunningScene running;

    test::TempDir dir;
    const std::string original = R"({"version": 1, "name": "File", "fields": {"density": 0.25, "color": [1, 0, 0]}})";
    const std::string path = dir.write("FogProfiles/file.fogprofile", original).generic_string();
    lua_pushstring(L, path.c_str());
    lua_setglobal(L, "PROFILE_PATH");

    const std::string script = R"(
        local night = FogProfile.builtin("night")
        assert(night ~= nil, "built-in night")
        assert(night.key == "builtin:night" and night.name == "Night" and night.readOnly)
        assert(night:isSet("attenuationDistance") and not night:isSet("density"))
        assert(math.abs(night:get("attenuationDistance") - 200) < 1e-4)
        assert(night:get("density") == nil, "not set: nil")
        assert(night:get("skyFill") == true)
        assert(FogProfile.builtin("nope") == nil)

        local ok, err = pcall(function() night:set("density", 1) end)
        assert(not ok and string.find(err, "read%-only"), "a built-in is read-only: " .. tostring(err))
        ok, err = pcall(function() night:get("volumetric") end)
        assert(not ok and string.find(err, "not a fog look field"), tostring(err))

        local file = FogProfile.load(PROFILE_PATH)
        assert(file ~= nil and file.name == "File" and file.readOnly)
        assert(math.abs(file:get("density") - 0.25) < 1e-6)
        ok = pcall(function() file:clear("density") end)
        assert(not ok, "a loaded profile is read-only")

        local copy = file:clone()
        assert(not copy.readOnly and copy.key ~= file.key)
        copy:set("density", 0.75)
        copy:set("emissive", Vector3(0.6, 1.4, 0))
        copy:set("colorMode", FogColorMode.LIT)
        copy:clear("color")
        assert(math.abs(copy:get("density") - 0.75) < 1e-6)
        assert(copy:isSet("emissive") and not copy:isSet("color"))
        assert(copy:get("colorMode") == FogColorMode.LIT)
        ok, err = pcall(function() copy:set("density", "thick") end)
        assert(not ok and string.find(err, "takes a number"), tostring(err))
        -- the loaded profile is as it was
        assert(math.abs(FogProfile.load(PROFILE_PATH):get("density") - 0.25) < 1e-6)
        assert(file:isSet("color"))
    )";
    EXPECT_EQ(fogProfileRunLua(L, script), "");
    // clone():set never touches the file
    EXPECT_EQ(dir.read("FogProfiles/file.fogprofile"), original);
    std::shared_ptr<const FogProfileData> fromFile = FogProfileLibrary::get(path);
    ASSERT_TRUE(fromFile);
    EXPECT_FLOAT_EQ(fromFile->values.density, 0.25f);
}

TEST(LuaFogProfileTest, FogBlendsAndScriptWritesShow){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    RunningScene running;
    Scene& scene = running.scene;

    FogProfileData red;
    FogComponent values;
    values.color = Vector3(1.0f, 0.0f, 0.0f);
    red.setFrom(values, FogField::Color);
    FogProfileLibrary::set("FogProfiles/red.fogprofile", red);

    Fog fog(&scene);
    scene.getComponent<FogComponent>(fog.getEntity()).color = Vector3(0.0f, 0.0f, 0.0f);
    FogZone zone(&scene);
    luabridge::setGlobal(L, fog, "fog");
    luabridge::setGlobal(L, zone, "zone");

    EXPECT_EQ(fogProfileRunLua(L, R"(
        assert(fog.profile == nil)
        assert(not fog.blending)
        fog:blendTo("FogProfiles/red.fogprofile", 2)
        assert(fog.blending)
        local c = fog:getResolved("color")
        assert(c.x < 1e-4, "starts from where it is: " .. c.x)
    )"), "");
    // game time moves the blend (RenderSystem::update)
    FogScene::advance(&scene, 1.0);
    EXPECT_EQ(fogProfileRunLua(L, R"(
        local c = fog:getResolved("color")
        -- half way in linear light, shown in sRGB as Fog's colours are
        assert(math.abs(c.x - 0.5 ^ (1 / 2.2)) < 1e-3, "half way: " .. c.x)
    )"), "");
    FogScene::advance(&scene, 1.5);
    EXPECT_EQ(fogProfileRunLua(L, R"(
        assert(not fog.blending)
        assert(fog.profile.key == "FogProfiles/red.fogprofile", "the blend's end is the runtime profile")
        assert(math.abs(fog:getResolved("color").x - 1) < 1e-4)

        -- a script's write shows over the profile, until cleared
        fog.color = Vector3(0, 0, 1)
        local c = fog:getResolved("color")
        assert(c.x < 1e-4 and math.abs(c.z - 1) < 1e-4, "the write wins")
        assert(math.abs(fog.color.z - 1) < 1e-4, "and reads back")
        fog:clearOverride("color")
        assert(math.abs(fog:getResolved("color").x - 1) < 1e-4, "the profile again")
        fog.density = 0.5
        fog:clearOverrides()

        -- a clone linked at run time; nil unlinks
        local mine = FogProfile.load("FogProfiles/red.fogprofile"):clone()
        mine:set("color", Vector3(0, 1, 0))
        fog.profile = mine
        assert(math.abs(fog:getResolved("color").y - 1) < 1e-4)
        mine:set("color", Vector3(1, 1, 0))
        assert(math.abs(fog:getResolved("color").x - 1) < 1e-4, "a clone's change shows at once")
        fog.profile = nil
        assert(fog.profile == nil and fog:getResolved("color").x < 1e-4)

        -- FogZone: every field
        zone.shape = FogZoneShape.SPHERE
        zone:setSize(4, 5, 6)
        zone.blendDistance = 0
        zone.priority = 7
        zone.weight = 0.5
        zone.profile = "FogProfiles/red.fogprofile"
        assert(zone.shape == FogZoneShape.SPHERE and zone.size.z == 6 and zone.blendDistance == 0)
        assert(zone.priority == 7 and zone.weight == 0.5 and zone.profile.key == "FogProfiles/red.fogprofile")
        -- the camera (none: the origin) is inside it: half the zone's red over the own black
        assert(math.abs(zone.currentWeight - 0.5) < 1e-6)
        zone.profile = nil
        assert(zone.profile == nil and zone.currentWeight == 0)
    )"), "");

    // none of it is in the Fog's saved fields
    const FogComponent& component = scene.getComponent<FogComponent>(fog.getEntity());
    EXPECT_TRUE(component.profile.path.empty());
    EXPECT_EQ(component.color, Vector3(0.0f, 0.0f, 0.0f));
    FogScene::resetRuntime(&scene);
    EXPECT_EQ(component.runtime.get(), nullptr);
}

}  // namespace lua_fog_profile_test
