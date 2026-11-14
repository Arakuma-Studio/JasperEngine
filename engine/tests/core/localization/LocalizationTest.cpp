// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// String Tables (docs/specs/S95-lua-runtime-gaps.md REQ-25 to REQ-27, AC-22): the CLDR plural
// rules, the .stringtable format and its ICU interchange form, the lookup's fallback chain and
// formatting, "Table:key", the language shared with Title Art and Remember Language Choice.

#include <gtest/gtest.h>

#include "Log.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"
#include "localization/Localization.h"
#include "localization/PluralRules.h"
#include "localization/StringTable.h"
#include "support/TestTempDir.h"
#include "util/TitleArtLanguage.h"
#include "TestSystem.h"

#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    class LocalizationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
            TitleArtLanguage::setProjectDefault("en");
            warnings.clear();
            listener = Log::addListener([this](const LogEntry& entry) {
                if (entry.level == LogLevel::Warn) warnings.push_back(entry.message);
            });
        }
        void TearDown() override {
            Log::removeListener(listener);
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
        }

        static StringTable parsed(const std::string& text) {
            StringTable table;
            std::string error;
            EXPECT_TRUE(StringTable::parse(text, table, &error)) << error;
            return table;
        }

        // UI: menu.play (en, es, pt), hud.coins plural (en, es, ru, ar, ja), greet ({name}), only.en
        static StringTable uiTable() {
            return parsed(R"({"version": 1, "entries": [
                {"key": "menu.play", "values": {"en": "Play", "es": "Jugar", "pt": "Jogar"}},
                {"key": "hud.coins", "comment": "HUD counter", "values": {
                    "en": {"one": "{count} coin", "other": "{count} coins"},
                    "es": {"one": "{count} moneda", "many": "{count} de monedas", "other": "{count} monedas"},
                    "ru": {"one": "{count} монета", "few": "{count} монеты", "many": "{count} монет", "other": "{count} монеты"},
                    "ar": {"zero": "z{count}", "one": "o{count}", "two": "t{count}", "few": "f{count}", "many": "m{count}", "other": "x{count}"},
                    "ja": {"other": "{count}枚"}}},
                {"key": "greet", "values": {"en": "Hello {name}, {0} and {1} {{braces}}"}},
                {"key": "only.en", "values": {"en": "English only"}}]})");
        }

        std::vector<std::string> warnings;
        LogListenerId listener = 0;
    };

    std::string coins(double count) {
        LocalizationArgs args;
        args.named.emplace_back("count", LocalizationArg::ofNumber(count));
        return Localization::get("hud.coins", args);
    }

}

TEST_F(LocalizationTest, PluralRulesOfTheTestedLanguages) {
    using C = PluralCategory;
    EXPECT_EQ(PluralRules::select("en", 1), C::ONE);
    EXPECT_EQ(PluralRules::select("en", 0), C::OTHER);
    EXPECT_EQ(PluralRules::select("en", 1.5), C::OTHER);
    EXPECT_EQ(PluralRules::select("en-GB", 2), C::OTHER);
    EXPECT_EQ(PluralRules::select("es", 1), C::ONE);
    EXPECT_EQ(PluralRules::select("es", 2), C::OTHER);
    EXPECT_EQ(PluralRules::select("es", 1000000), C::MANY);
    EXPECT_EQ(PluralRules::select("ru", 1), C::ONE);
    EXPECT_EQ(PluralRules::select("ru", 21), C::ONE);
    EXPECT_EQ(PluralRules::select("ru", 11), C::MANY);
    EXPECT_EQ(PluralRules::select("ru", 3), C::FEW);
    EXPECT_EQ(PluralRules::select("ru", 13), C::MANY);
    EXPECT_EQ(PluralRules::select("ru", 25), C::MANY);
    EXPECT_EQ(PluralRules::select("ru", 2.5), C::OTHER);
    EXPECT_EQ(PluralRules::select("ar", 0), C::ZERO);
    EXPECT_EQ(PluralRules::select("ar", 1), C::ONE);
    EXPECT_EQ(PluralRules::select("ar", 2), C::TWO);
    EXPECT_EQ(PluralRules::select("ar", 5), C::FEW);
    EXPECT_EQ(PluralRules::select("ar", 105), C::FEW);
    EXPECT_EQ(PluralRules::select("ar", 11), C::MANY);
    EXPECT_EQ(PluralRules::select("ar", 100), C::OTHER);
    EXPECT_EQ(PluralRules::select("ja", 1), C::OTHER);
    EXPECT_EQ(PluralRules::select("fr", 0), C::ONE);
    EXPECT_EQ(PluralRules::select("pt-BR", 0), C::ONE);
    EXPECT_EQ(PluralRules::select("pt-PT", 0), C::OTHER);
    EXPECT_EQ(PluralRules::select("pl", 22), C::FEW);
    EXPECT_EQ(PluralRules::select("pl", 12), C::MANY);
    // an unknown language: one and other
    EXPECT_EQ(PluralRules::select("tlh", 1), C::ONE);
    EXPECT_EQ(PluralRules::select("tlh", 7), C::OTHER);
    EXPECT_EQ(PluralRules::categories("ja"), std::vector<PluralCategory>({C::OTHER}));
    EXPECT_EQ(PluralRules::categories("ar").size(), 6u);
}

TEST_F(LocalizationTest, TableRoundTripsAndReportsBadEntries) {
    const StringTable table = uiTable();
    ASSERT_EQ(table.entries.size(), 4u);
    const StringTable again = parsed(table.write());
    EXPECT_TRUE(again == table);
    EXPECT_EQ(again.find("hud.coins")->comment, "HUD counter");

    StringTable bad;
    std::vector<std::string> problems;
    std::string error;
    ASSERT_TRUE(StringTable::parse(R"({"version": 1, "entries": [{"key": "a", "values": {"en": "A"}}, {"key": "a"},
        {"key": "x:y"}, {"key": "b", "values": {"en": {"lots": "?"}, "!!": "?"}}]})", bad, &error, &problems));
    EXPECT_EQ(bad.entries.size(), 2u);
    EXPECT_EQ(problems.size(), 4u);
    EXPECT_FALSE(StringTable::parse("[]", bad, &error));
    EXPECT_FALSE(StringTable::parse(R"({"version": 2, "entries": []})", bad, &error));
    EXPECT_NE(error.find("version 2"), std::string::npos);
}

TEST_F(LocalizationTest, PluralTravelsAsIcuMessageFormat) {
    const StringTableValue* value = uiTable().find("hud.coins")->find("en");
    ASSERT_NE(value, nullptr);
    const std::string icu = value->toMessageFormat();
    EXPECT_EQ(icu, "{count, plural, one {{count} coin} other {{count} coins}}");
    EXPECT_TRUE(StringTableValue::fromMessageFormat(icu) == *value);
    // a translation tool's '#'
    const StringTableValue hash = StringTableValue::fromMessageFormat("{count, plural, one {# moneda} other {# monedas}}");
    EXPECT_EQ(hash.form(PluralCategory::ONE), "{count} moneda");
    // a text that only looks like one stays text
    EXPECT_FALSE(StringTableValue::fromMessageFormat("{name} won").plural);
}

TEST_F(LocalizationTest, FallsBackToBaseThenDefaultThenKey) {
    Localization::setTablesForTesting({{"UI", uiTable()}});
    ASSERT_TRUE(TitleArtLanguage::setGlobal("pt-BR"));
    EXPECT_EQ(Localization::get("menu.play"), "Jogar");       // the base language
    EXPECT_EQ(Localization::get("only.en"), "English only");  // the project default
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(Localization::get("no.such.key"), "no.such.key");
    EXPECT_EQ(Localization::get("no.such.key"), "no.such.key");
    EXPECT_EQ(warnings.size(), 1u) << "warned once per key";
    // without a default value either: the key
    TitleArtLanguage::setProjectDefault("es");
    EXPECT_EQ(Localization::get("only.en"), "only.en");
}

TEST_F(LocalizationTest, FormatsPositionalNamedAndBraces) {
    Localization::setTablesForTesting({{"UI", uiTable()}});
    LocalizationArgs args;
    args.positional = {LocalizationArg::ofText("Ana"), LocalizationArg::ofNumber(2.5)};
    args.named.emplace_back("name", LocalizationArg::ofText("Bo"));
    EXPECT_EQ(Localization::get("greet", args), "Hello Bo, Ana and 2.5 {braces}");
    EXPECT_EQ(Localization::format("{missing} {0}", LocalizationArgs()), "{missing} {0}");
    EXPECT_EQ(LocalizationArg::ofNumber(3).text, "3");
    // data-loc-args's JSON forms
    EXPECT_EQ(LocalizationArgs::fromJson(R"({"count": 3})").find("count")->text, "3");
    EXPECT_EQ(LocalizationArgs::fromJson(R"(["a", 2])").positional.size(), 2u);
    EXPECT_EQ(LocalizationArgs::fromJson("not json").positional.size(), 0u);
    EXPECT_EQ(LocalizationArgs::fromJson(args.toJson()).find("name")->text, "Bo");
}

TEST_F(LocalizationTest, PluralFormsFollowTheLanguagesRules) {
    Localization::setTablesForTesting({{"UI", uiTable()}});
    EXPECT_EQ(coins(1), "1 coin");
    EXPECT_EQ(coins(3), "3 coins");
    TitleArtLanguage::setGlobal("es");
    EXPECT_EQ(coins(1), "1 moneda");
    EXPECT_EQ(coins(3), "3 monedas");
    EXPECT_EQ(coins(1000000), "1000000 de monedas");
    TitleArtLanguage::setGlobal("ru");
    EXPECT_EQ(coins(21), "21 монета");
    EXPECT_EQ(coins(3), "3 монеты");
    EXPECT_EQ(coins(5), "5 монет");
    TitleArtLanguage::setGlobal("ar");
    EXPECT_EQ(coins(0), "z0");
    EXPECT_EQ(coins(2), "t2");
    EXPECT_EQ(coins(11), "m11");
    TitleArtLanguage::setGlobal("ja");
    EXPECT_EQ(coins(1), "1枚");
    // the count from {0} when no `count`; no number: `other`
    TitleArtLanguage::setGlobal("en");
    LocalizationArgs positional;
    positional.positional.push_back(LocalizationArg::ofNumber(1));
    EXPECT_EQ(Localization::get("hud.coins", positional), "{count} coin");
    EXPECT_EQ(Localization::get("hud.coins"), "{count} coins");
}

TEST_F(LocalizationTest, TableKeyAndDuplicateKeys) {
    StringTable other = parsed(R"({"version": 1, "entries": [{"key": "menu.play", "values": {"en": "Start"}}]})");
    Localization::setTablesForTesting({{"UI", uiTable()}, {"Other", other}});
    EXPECT_EQ(Localization::get("menu.play"), "Play") << "a bare key: the first table that has it";
    EXPECT_EQ(Localization::get("Other:menu.play"), "Start");
    EXPECT_EQ(Localization::get("UI:menu.play"), "Play");
    EXPECT_TRUE(Localization::has("Other:menu.play"));
    EXPECT_FALSE(Localization::has("Other:greet"));
}

TEST_F(LocalizationTest, LanguageIsTitleArtsLanguage) {
    Localization::configure({{"en", "es"}, true, {}});
    Localization::setTablesForTesting({{"UI", uiTable()}});
    EXPECT_EQ(Localization::getLanguage(), "en") << "the test device is en-US";
    const uint64_t before = Localization::generation();
    std::string heard;
    const int id = TitleArtLanguage::addListener([&](const std::string& code) { heard = code; });
    ASSERT_TRUE(Localization::setLanguage("es-MX"));
    EXPECT_EQ(TitleArtLanguage::global(), "es-MX") << "one value for titles and strings";
    EXPECT_EQ(heard, "es-MX");
    EXPECT_EQ(Localization::getLanguage(), "es") << "matched against the project's languages";
    EXPECT_NE(Localization::generation(), before);
    EXPECT_EQ(Localization::get("menu.play"), "Jugar");
    // TitleArt.setLanguage moves strings too
    TitleArtLanguage::setGlobal("en");
    EXPECT_EQ(Localization::get("menu.play"), "Play");
    std::string error;
    EXPECT_FALSE(Localization::setLanguage("not a tag", &error));
    EXPECT_FALSE(error.empty());
    TitleArtLanguage::removeListener(id);
    EXPECT_EQ(Localization::getLanguages(), std::vector<std::string>({"en", "es"}));
}

TEST_F(LocalizationTest, ReadsTablesFromTheirFiles) {
    TestTempDir dir{"localization"};
    test::TestSystem& system = test::TestSystem::get();
    system.setAssetPath(dir.path().string());
    {
        std::ofstream out(dir.path() / "Menu.stringtable", std::ios::binary);
        out << uiTable().write();
    }
    Localization::configure({{}, true, {"Menu.stringtable"}});
    EXPECT_EQ(Localization::get("Menu:menu.play"), "Play");
    // no project languages: the tables' languages
    EXPECT_EQ(Localization::getLanguages().size(), 6u);
    system.setAssetPath("");
}

TEST_F(LocalizationTest, RemembersTheChosenLanguage) {
    TestTempDir dir{"localization-remember"};
    test::TestSystem& system = test::TestSystem::get();
    UserSettingsStore::instance().unload();
    system.setUserDataPath(dir.path().string());

    Localization::configure({{"en", "es"}, true, {}});
    Localization::start();
    EXPECT_EQ(TitleArtLanguage::global(), "");
    Localization::setLanguage("es");
    Localization::stop();
    EXPECT_EQ(UserSettings::getStringForKey(Localization::REMEMBERED_LANGUAGE_KEY), "es");
    // the next start: the stored choice first
    TitleArtLanguage::setGlobal("");
    Localization::start();
    EXPECT_EQ(TitleArtLanguage::global(), "es");
    // back to the device's language forgets it
    Localization::setLanguage("");
    EXPECT_FALSE(UserSettings::hasKey(Localization::REMEMBERED_LANGUAGE_KEY));
    Localization::stop();

    // Remember Language Choice off: nothing stored, nothing applied
    UserSettings::setStringForKey(Localization::REMEMBERED_LANGUAGE_KEY, "es");
    TitleArtLanguage::setGlobal("");
    Localization::configure({{"en", "es"}, false, {}});
    Localization::start();
    EXPECT_EQ(TitleArtLanguage::global(), "");
    Localization::setLanguage("en");
    EXPECT_EQ(UserSettings::getStringForKey(Localization::REMEMBERED_LANGUAGE_KEY), "es");
    Localization::stop();

    UserSettings::deleteAll();
    UserSettingsStore::instance().unload();
    system.setUserDataPath("");
}
