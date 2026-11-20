// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Localization table (docs/specs/game-ui-localization.md R17, AC11): every member of the
// hand-written stub exists, onLanguageChanged fires once per change, and Stop clears it.

#include "Engine.h"
#include "localization/Localization.h"
#include "localization/StringTableRuntime.h"
#include "script/LuaBinding.h"
#include "util/TitleArtLanguage.h"

#include "TestPaths.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace jasperengine;

namespace {

    std::string runLua(lua_State* L, const std::string& code) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    class LocalizationLua : public ::testing::Test {
    protected:
        void SetUp() override {
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            Localization::registerReader(".mem", [](const std::string& path, const std::string& language, std::string*) {
                std::vector<l10n::StringTable::Entry> entries;
                if (path == "ui.en.mem") entries = {{"play", "Play"}, {"coins", "{n, plural, one {# coin} other {# coins}}"}};
                if (path == "ui.de.mem") entries = {{"play", "Spielen"}};
                return l10n::StringTable::fromEntries(language, entries);
            });
            LocalizationManifest manifest;
            manifest.defaultLanguage = "en";
            manifest.rememberChoice = false;
            manifest.languages = {"en", "de"};
            manifest.names = {{"en", "English"}, {"de", "Deutsch"}};
            manifest.namespaces.push_back({"ui", "en", {{"en", "ui.en.mem"}, {"de", "ui.de.mem"}}});
            Localization::setManifest(manifest);
        }

        void TearDown() override {
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
            runLua(LuaBinding::getLuaState(), "localizationEvents = nil");
        }
    };

}

TEST_F(LocalizationLua, ApiMatchesStubs) {
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    std::ifstream file(test::testPaths().sourceDir + "/../../core/script/binding/stubs/Localization.lua");
    ASSERT_TRUE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string stub = text.str();
    std::set<std::string> members;
    const std::regex function(R"(function Localization\.(\w+)\()");
    for (std::sregex_iterator it(stub.begin(), stub.end(), function), end; it != end; ++it) members.insert((*it)[1]);
    // the fields of the Localization class itself
    const size_t classAt = stub.find("---@class Localization\n");
    ASSERT_NE(classAt, std::string::npos);
    const std::string fields = stub.substr(classAt, stub.find("Localization = {}") - classAt);
    const std::regex field(R"(---@field (\w+))");
    for (std::sregex_iterator it(fields.begin(), fields.end(), field), end; it != end; ++it) members.insert((*it)[1]);
    EXPECT_EQ(members.size(), 10u);
    for (const std::string& member : members) {
        EXPECT_EQ(runLua(L, "assert(Localization." + member + " ~= nil, '" + member + "')"), "") << member;
    }
    EXPECT_EQ(runLua(L,
        "assert(Localization.get('ui.play') == 'Play')\n"
        "assert(Localization.get('ui.coins', {n = 2}) == '2 coins')\n"
        "assert(Localization.get('ui.nope', nil, 'Fallback') == 'Fallback')\n"
        "assert(Localization.format('{n, number}', {n = 1234.5}, 'de') == '1.234,5')\n"
        "assert(Localization.has('ui.play') and not Localization.has('ui.nope'))\n"
        "assert(Localization.language == 'en')\n"
        "assert(#Localization.languages == 2 and Localization.languages[2] == 'de')\n"
        "assert(Localization.getLanguageName('de') == 'Deutsch')\n"
        "assert(Localization.isRightToLeft('he') and not Localization.isRightToLeft())\n"
        "assert(Localization.setLanguage('de') and Localization.get('ui.play') == 'Spielen')\n"
        "assert(Localization.setLanguage('bad tag!') == false and Localization.language == 'de')\n"
        "assert(type(Localization.scan().literal) == 'table')\n"
        "assert(not pcall(function() Localization.language = 'fr' end))\n"), "");
}

TEST_F(LocalizationLua, OnLanguageChangedFiresOnce) {
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(runLua(L,
        "localizationEvents = {}\n"
        "Localization.onLanguageChanged = function(code) table.insert(localizationEvents, code) end\n"
        "Localization.setLanguage('de')\n"
        "Localization.setLanguage('de')\n"), "");
    ASSERT_TRUE(TitleArtLanguage::setGlobal("en"));
    EXPECT_EQ(runLua(L, "assert(#localizationEvents == 2, #localizationEvents)\n"
                        "assert(localizationEvents[1] == 'de' and localizationEvents[2] == 'en')\n"), "");
}

TEST_F(LocalizationLua, EventIsClearedAtStop) {
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(runLua(L,
        "localizationEvents = {}\n"
        "Localization.onLanguageChanged = function(code) table.insert(localizationEvents, code) end\n"), "");
    EXPECT_TRUE(Localization::onLanguageChanged.hasSubscribers());
    Engine::clearAllSubscriptions(false);
    EXPECT_FALSE(Localization::onLanguageChanged.hasSubscribers());
    ASSERT_TRUE(Localization::setLanguage("de"));
    EXPECT_EQ(runLua(L, "assert(#localizationEvents == 0)"), "");
}
