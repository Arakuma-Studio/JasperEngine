// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// String Tables in HTML UI (docs/specs/S95-lua-runtime-gaps.md REQ-28, AC-22): data-loc texts at
// load (before onLoad), again on a language change, and el:setLocalized from Lua.

#include "TestScriptCommon.h"

#include "localization/Localization.h"
#include "localization/StringTable.h"
#include "util/TitleArtLanguage.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class HtmlUILocalizationTest : public ScriptTest {
    protected:
        const char* documentFile() const override { return ""; }

        void SetUp() override{
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
            TitleArtLanguage::setProjectDefault("en");
            StringTable table;
            std::string error;
            ASSERT_TRUE(StringTable::parse(R"({"version": 1, "entries": [
                {"key": "menu.play", "values": {"en": "Play", "es": "Jugar"}},
                {"key": "hud.coins", "values": {"en": {"one": "{count} coin", "other": "{count} coins"},
                                                "es": {"one": "{count} moneda", "other": "{count} monedas"}}}]})", table, &error)) << error;
            Localization::setTablesForTesting({{"UI", table}});
            ScriptTest::SetUp();
        }

        void TearDown() override{
            ScriptTest::TearDown();
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
        }

        std::string text(const char* id){
            return document->getElementById(id).getTextContent();
        }
    };

}

TEST_F(HtmlUILocalizationTest, DataLocFollowsTheLanguage){
    ASSERT_TRUE(document->load("s95/localized.html"));
    EXPECT_EQ(text("title"), "Play");
    EXPECT_EQ(text("coins"), "3 coins");
    EXPECT_EQ(text("plain"), "plain");

    TitleArtLanguage::setGlobal("es");
    update();
    EXPECT_EQ(text("title"), "Jugar");
    EXPECT_EQ(text("coins"), "3 monedas");
}

TEST_F(HtmlUILocalizationTest, SetLocalizedFromLua){
    ASSERT_TRUE(document->load("s95/localized.html"));
    setDocumentGlobal();
    EXPECT_LUA(R"(doc:getElementById("plain"):setLocalized("hud.coins", {count = 1}))");
    EXPECT_EQ(text("plain"), "1 coin");
    EXPECT_EQ(luaString(R"(doc:getElementById("plain"):getAttribute("data-loc"))"), "hud.coins");
    TitleArtLanguage::setGlobal("es");
    update();
    EXPECT_EQ(text("plain"), "1 moneda");
    // a value Lua cannot pass raises, nothing changes
    EXPECT_NE(lua(R"(doc:getElementById("plain"):setLocalized("menu.play", {{}}))"), "");
    EXPECT_EQ(text("plain"), "1 moneda");
    EXPECT_EQ(luaString(R"(Localization.get("hud.coins", {count = 2}))"), "2 monedas");
    EXPECT_EQ(luaString("Localization.language"), "es");
}
