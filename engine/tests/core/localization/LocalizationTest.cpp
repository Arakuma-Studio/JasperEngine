// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The game language and String Table lookups (docs/specs/game-ui-localization.md R6-R10, R13,
// R19, R20, R27, AC3, AC10, AC12, AC15): resolution with Title Art's rules, the fallback chain,
// unknown keys in release and development builds, the shared global with TitleArt.setLanguage,
// the inactive cost, capture, the OS locale event, pseudo-locales and the cooked tables.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "pixelart/BitmapFont.h"
#include "subsystem/UISystem.h"
#include "io/UserSettings.h"
#include "localization/Localization.h"
#include "localization/StringTableRuntime.h"
#include "util/TitleArtLanguage.h"

#include "LogCapture.h"
#include "TempDir.h"
#include "TestSystem.h"
#include "support/AllocationCounter.h"

#include <map>
#include <string>
#include <vector>

namespace LocalizationTest_cpp {

using namespace jasperengine;
using namespace jasperengine::l10n;
using jasperengine::test::LogCapture;
using jasperengine::test::TempDir;
using jasperengine::test::TestSystem;

namespace {

    // tables in memory, read through a registered reader (".mem" files)
    std::map<std::string, std::vector<StringTable::Entry>>& memoryTables() {
        static std::map<std::string, std::vector<StringTable::Entry>> tables;
        return tables;
    }

    class LocalizationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            UserSettings::removeKey("jasper.language");
            TestSystem::get().preferredLocales.clear();
            wasDevelopment = Engine::isDevelopmentBuild();
            Engine::setDevelopmentBuild(true);
            memoryTables().clear();
        }

        void TearDown() override {
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
            UserSettings::removeKey("jasper.language");
            TestSystem::get().preferredLocales.clear();
            Engine::setDevelopmentBuild(wasDevelopment);
            memoryTables().clear();
        }

        // one namespace `ui` whose languages are the given tables; the first is the source
        void install(const std::string& source, const std::string& defaultLanguage,
                     const std::vector<std::pair<std::string, std::vector<StringTable::Entry>>>& languages,
                     const std::vector<std::string>& available = {}) {
            Localization::registerReader(".mem", [](const std::string& path, const std::string& language, std::string*) {
                return StringTable::fromEntries(language, memoryTables()[path]);
            });
            LocalizationManifest manifest;
            manifest.defaultLanguage = defaultLanguage;
            LocalizationNamespace ns;
            ns.name = "ui";
            ns.sourceLanguage = source;
            for (const auto& [language, entries] : languages) {
                memoryTables()["ui." + language + ".mem"] = entries;
                ns.files[language] = "ui." + language + ".mem";
                manifest.languages.push_back(language);
            }
            if (!available.empty()) manifest.languages = available;
            manifest.namespaces.push_back(ns);
            Localization::setManifest(manifest);
        }

        bool wasDevelopment = false;
    };

}

TEST_F(LocalizationTest, ResolvesWithTitleArtRules) {
    TestSystem::get().preferredLocales = {"zh-HK", "en-US"};
    install("en", "en", {{"en", {{"hello", "Hello"}}}, {"zh-Hant", {{"hello", "你好"}}}, {"pt-BR", {{"hello", "Olá"}}}, {"de", {{"hello", "Hallo"}}}});
    EXPECT_EQ(Localization::getLanguage(), "zh-Hant");
    EXPECT_EQ(Localization::get("ui.hello"), "你好");

    // pt-PT on the device picks pt-BR when only that exists
    TestSystem::get().preferredLocales = {"pt-PT"};
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "pt-BR");

    // the remembered choice beats the device
    UserSettings::setStringForKey("jasper.language", "de");
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "de");

    // an explicit choice beats both and is remembered; "" goes back to the device
    EXPECT_TRUE(Localization::setLanguage("zh-TW"));
    EXPECT_EQ(Localization::getLanguage(), "zh-Hant");
    EXPECT_EQ(UserSettings::getStringForKey("jasper.language", ""), "zh-TW");
    EXPECT_TRUE(Localization::setLanguage(""));
    EXPECT_EQ(Localization::getLanguage(), "pt-BR");
    EXPECT_FALSE(UserSettings::hasKey("jasper.language"));
    // an invalid tag changes nothing
    std::string error;
    EXPECT_FALSE(Localization::setLanguage("not a tag!", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Localization::getLanguage(), "pt-BR");
    // nothing matches: the default
    TestSystem::get().preferredLocales = {"ko-KR"};
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "en");
    EXPECT_EQ(Localization::getLanguages(), (std::vector<std::string>{"en", "zh-Hant", "pt-BR", "de"}));
}

TEST_F(LocalizationTest, FallbackChain) {
    // source en, default es; pt-BR -> pt -> es -> the source text, per key
    install("en", "es",
            {{"en", {{"a", "A en"}, {"b", "B en"}, {"c", "C en"}, {"d", "D en"}, {"e", "E en"}}},
             {"es", {{"a", "A es"}, {"b", "B es"}, {"c", "C es"}}},
             {"pt", {{"a", "A pt"}, {"b", "B pt"}}},
             {"pt-BR", {{"a", "A br"}, {"e", "E br", true}}}});
    ASSERT_TRUE(Localization::setLanguage("pt-BR"));
    EXPECT_EQ(Localization::get("ui.a"), "A br");
    EXPECT_EQ(Localization::get("ui.b"), "B pt");
    EXPECT_EQ(Localization::get("ui.c"), "C es");
    EXPECT_EQ(Localization::get("ui.d"), "D en");
    // a needs_review translation is still shown
    EXPECT_EQ(Localization::get("ui.e"), "E br");
    EXPECT_TRUE(Localization::has("ui.d"));
    EXPECT_FALSE(Localization::has("ui.zzz"));
    EXPECT_EQ(Localization::getView("ui.a"), "A br");
    // the text is formatted in the language that supplied it
    install("en", "en", {{"en", {{"coins", "{n, plural, one {# coin} other {# coins}}"}}}, {"de", {{"other", "x"}}}});
    ASSERT_TRUE(Localization::setLanguage("de"));
    EXPECT_EQ(Localization::get("ui.coins", {{"n", int64_t(1)}}), "1 coin");
    EXPECT_EQ(Localization::get("ui.coins", {{"n", int64_t(1500)}}), "1,500 coins");
}

TEST_F(LocalizationTest, UnknownKeyNeverShowsTheKeyInRelease) {
    install("en", "en", {{"en", {{"play", "Play"}}}, {"de", {{"play", "Spielen"}}}});
    LogCapture log;
    Engine::setDevelopmentBuild(false);
    EXPECT_EQ(Localization::get("ui.nope"), "");
    EXPECT_EQ(Localization::get("ui.nope", {}, "Fallback"), "Fallback");
    EXPECT_EQ(Localization::get("nonamespace"), "");
    Engine::setDevelopmentBuild(true);
    EXPECT_EQ(Localization::get("ui.nope"), "\xE2\x9F\xA6ui.nope\xE2\x9F\xA7");
    // one warning per key and language
    EXPECT_EQ(log.count(D_LOG_WARN, "ui.nope"), 1u);
    ASSERT_TRUE(Localization::setLanguage("de"));
    EXPECT_EQ(Localization::get("ui.nope"), "\xE2\x9F\xA6ui.nope\xE2\x9F\xA7");
    EXPECT_EQ(log.count(D_LOG_WARN, "ui.nope"), 2u);
    // a missing argument prints its name only in development
    install("en", "en", {{"en", {{"hi", "Hi {name}"}}}});
    EXPECT_EQ(Localization::get("ui.hi"), "Hi {name}");
    Engine::setDevelopmentBuild(false);
    EXPECT_EQ(Localization::get("ui.hi"), "Hi ");
}

TEST_F(LocalizationTest, TitleArtSetLanguageDrivesLocalization) {
    install("en", "en", {{"en", {{"play", "Play"}}}, {"de", {{"play", "Spielen"}}}, {"ja", {{"play", "プレイ"}}}});
    std::vector<std::string> events;
    Localization::onLanguageChanged.add("test", [&events](const std::string& code) { events.push_back(code); });
    // TitleArt.setLanguage sets the same global
    ASSERT_TRUE(TitleArtLanguage::setGlobal("de"));
    EXPECT_EQ(Localization::getLanguage(), "de");
    EXPECT_EQ(Localization::get("ui.play"), "Spielen");
    ASSERT_TRUE(Localization::setLanguage("ja"));
    EXPECT_EQ(TitleArtLanguage::global(), "ja");
    EXPECT_EQ(Localization::get("ui.play"), "プレイ");
    // the same value again is no change
    ASSERT_TRUE(Localization::setLanguage("ja"));
    EXPECT_EQ(events, (std::vector<std::string>{"de", "ja"}));
    EXPECT_EQ(Localization::getLanguageName("ja"), "ja");
    EXPECT_TRUE(Localization::isRightToLeft("ar-EG"));
    EXPECT_TRUE(Localization::isRightToLeft("ar-XB"));
    EXPECT_TRUE(Localization::isRightToLeft("az-Arab"));
    EXPECT_FALSE(Localization::isRightToLeft("az-Latn"));
    EXPECT_FALSE(Localization::isRightToLeft("ja"));
}

TEST_F(LocalizationTest, InactiveCostsNothing) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    Engine::setDevelopmentBuild(false);
    std::string out;
    testsupport::AllocationCountScope allocations;
    for (int frame = 0; frame < 600; frame++) {
        EXPECT_FALSE(Localization::isActive());
        EXPECT_FALSE(Localization::has("ui.play"));
        EXPECT_FALSE(Localization::translate("ui.play", {}, out));
        EXPECT_TRUE(Localization::getView("ui.play").empty());
        EXPECT_TRUE(Localization::get("ui.play").empty());
        EXPECT_TRUE(Localization::getLanguage().empty());
        EXPECT_FALSE(Localization::wasProduced("Play"));
    }
    // an empty manifest keeps it inactive and reads nothing
    Localization::setManifest(LocalizationManifest());
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_FALSE(Localization::isActive());
}

TEST_F(LocalizationTest, CaptureReportsUntranslatedUnknownAndStale) {
#if JASPERENGINE_L10N_CAPTURE
    install("en", "en", {{"en", {{"a", "A"}, {"b", "B"}, {"c", "C"}}}, {"de", {{"a", "A de"}, {"c", "C de", true}}}});
    ASSERT_TRUE(Localization::setLanguage("de"));
    std::string out;
    Localization::translate("ui.a", {}, out, "#play");
    Localization::translate("ui.b", {}, out, "#options");
    Localization::translate("ui.b", {}, out, "#options");
    Localization::translate("ui.c", {}, out, "Title");
    Localization::translate("ui.zzz", {}, out, "#missing");
    const std::vector<Localization::Miss> misses = Localization::getMisses();
    ASSERT_EQ(misses.size(), 3u);
    auto find = [&misses](const std::string& kind) -> const Localization::Miss* {
        for (const auto& miss : misses) if (miss.kind == kind) return &miss;
        return nullptr;
    };
    ASSERT_NE(find("untranslated"), nullptr);
    EXPECT_EQ(find("untranslated")->key, "ui.b");
    EXPECT_EQ(find("untranslated")->where, "#options");
    EXPECT_EQ(find("untranslated")->count, 2u);
    EXPECT_EQ(find("untranslated")->language, "de");
    ASSERT_NE(find("stale"), nullptr);
    EXPECT_EQ(find("stale")->key, "ui.c");
    ASSERT_NE(find("unknown_key"), nullptr);
    EXPECT_EQ(find("unknown_key")->where, "#missing");
    // the produced texts are remembered; others are literal
    EXPECT_TRUE(Localization::wasProduced("A de"));
    EXPECT_FALSE(Localization::wasProduced("Options"));
    Localization::clearMisses();
    EXPECT_TRUE(Localization::getMisses().empty());
#else
    FAIL() << "the core tests build with JASPERENGINE_L10N_CAPTURE";
#endif
}

TEST_F(LocalizationTest, LocaleEventReresolvesUnlessSetExplicitly) {
    TestSystem::get().preferredLocales = {"de-DE"};
    install("en", "en", {{"en", {{"play", "Play"}}}, {"de", {{"play", "Spielen"}}}, {"fr", {{"play", "Jouer"}}}});
    EXPECT_EQ(Localization::getLanguage(), "de");
    int events = 0;
    Localization::onLanguageChanged.add("test", [&events](const std::string&) { events++; });
    LogCapture log;
    TestSystem::get().preferredLocales = {"fr-FR"};
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "fr");
    EXPECT_EQ(events, 1);
    EXPECT_TRUE(log.contains("[Localization] language fr"));
    // a game that chose a language keeps it
    ASSERT_TRUE(Localization::setLanguage("de"));
    EXPECT_EQ(events, 2);
    TestSystem::get().preferredLocales = {"en-US"};
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "de");
    EXPECT_EQ(events, 2);
}

TEST_F(LocalizationTest, PseudoLocalesTransformTheDefaultText) {
    install("en", "en", {{"en", {{"play", "Play"}, {"coins", "{n, plural, one {# coin} other {# coins}}"}}}, {"de", {{"play", "Spielen"}}}});
    ASSERT_TRUE(Localization::setLanguage("en-XA"));
    EXPECT_EQ(Localization::getLanguage(), "en-XA");
    // accented, 40 % longer (4 code points -> 2 fillers), bracketed
    EXPECT_EQ(Localization::get("ui.play"), "\xE2\x9F\xA6\xC3\x9E\xC4\xBA\xC3\xA1\xC3\xBD\xC2\xB7\xC2\xB7\xE2\x9F\xA7");
    // arguments and syntax stay; only literal text changes
    const std::string coins = Localization::get("ui.coins", {{"n", int64_t(3)}});
    EXPECT_NE(coins.find("3 "), std::string::npos) << coins;
    ASSERT_TRUE(Localization::setLanguage("ar-XB"));
    EXPECT_EQ(Localization::get("ui.play"), "yalP");
    EXPECT_TRUE(Localization::isRightToLeft(""));
    // never in release
    Engine::setDevelopmentBuild(false);
    TitleArtLanguage::localesChanged();
    EXPECT_EQ(Localization::getLanguage(), "en");
}

TEST_F(LocalizationTest, LoadsJsonAndCookedTablesLazily) {
    TempDir dir;
    const std::string source = dir.write("l10n/ui.stringtable",
        R"({"format": "jasperengine.stringtable", "version": 1, "guid": "g1", "namespace": "ui", "source_language": "en",)"
        R"( "entries": [{"key": "menu.play", "text": "Play"}, {"key": "menu.quit", "text": "Quit"}]})").generic_string();
    const std::string de = dir.write("l10n/ui.de.translation",
        R"({"format": "jasperengine.translation", "version": 1, "guid": "g2", "owner": {"guid": "g1", "path": "l10n/ui.stringtable"},)"
        R"( "language": "de", "entries": {"menu.play": {"text": "Spielen", "status": "translated", "source_hash": "00000000"}}})").generic_string();
    std::string cooked;
    std::string error;
    ASSERT_TRUE(StringTable::cook("fr", {{"menu.play", "Jouer"}, {"menu.quit", "Quitter"}}, cooked, &error)) << error;
    const std::string fr = dir.write("l10n/ui.fr.strbin", cooked).generic_string();
    LocalizationManifest manifest;
    manifest.defaultLanguage = "en";
    manifest.languages = {"en", "de", "fr"};
    manifest.namespaces.push_back({"ui", "en", {{"en", source}, {"de", de}, {"fr", fr}}});
    // the manifest round-trips through its JSON
    LocalizationManifest reread;
    ASSERT_TRUE(LocalizationManifest::fromJson(manifest.toJson(), reread, &error)) << error;
    EXPECT_EQ(reread.toJson(), manifest.toJson());
    Localization::setManifest(reread);
    ASSERT_TRUE(Localization::setLanguage("de"));
    EXPECT_EQ(Localization::get("ui.menu.play"), "Spielen");
    EXPECT_EQ(Localization::get("ui.menu.quit"), "Quit");
    ASSERT_TRUE(Localization::setLanguage("fr"));
    EXPECT_EQ(Localization::get("ui.menu.quit"), "Quitter");
}

TEST_F(LocalizationTest, CookedLookupMatchesJson) {
    // every key of 12 languages: the cooked table finds what the entries hold
    const char* languages[] = {"en", "de", "fr", "es", "it", "pt-BR", "ja", "ko", "zh-Hans", "ru", "ar", "pl"};
    for (const char* language : languages) {
        std::vector<StringTable::Entry> entries;
        for (int k = 0; k < 500; k++) {
            entries.push_back({"key." + std::to_string(k), std::string(language) + " text " + std::to_string(k) + (k % 7 == 0 ? " {n}" : ""),
                               k % 11 == 0});
        }
        std::string cooked;
        std::string error;
        ASSERT_TRUE(StringTable::cook(language, entries, cooked, &error)) << error;
        const auto table = StringTable::fromStrbin(cooked, &error);
        ASSERT_TRUE(table) << error;
        EXPECT_EQ(table->language(), language);
        EXPECT_EQ(table->size(), entries.size());
        for (const auto& entry : entries) {
            std::string_view text;
            bool review = false;
            bool plain = false;
            ASSERT_TRUE(table->find(entry.key, text, &review, &plain)) << entry.key;
            EXPECT_EQ(text, entry.text);
            EXPECT_EQ(review, entry.needsReview);
            EXPECT_EQ(plain, entry.text.find('{') == std::string::npos);
        }
        std::string_view text;
        EXPECT_FALSE(table->find("key.missing", text));
        EXPECT_EQ(table->memoryBytes(), table->size() * 16 + [&] { size_t n = 0; for (const auto& e : entries) n += e.text.size(); return n; }());
    }
    // a damaged file is refused, never read past
    std::string cooked;
    ASSERT_TRUE(StringTable::cook("en", {{"a", "A"}, {"b", "B"}}, cooked, nullptr));
    for (size_t cut = 0; cut < cooked.size(); cut++) {
        std::string error;
        EXPECT_FALSE(StringTable::fromStrbin(std::string_view(cooked).substr(0, cut), &error)) << cut;
    }
}

TEST_F(LocalizationTest, HashCollisionIsAnExportError) {
    // a forced collision: the same hash for two keys (a key written twice hashes alike)
    std::string cooked;
    std::string error;
    EXPECT_FALSE(StringTable::cook("en", {{"menu.play", "Play"}, {"menu.quit", "Quit"}, {"menu.play", "Again"}}, cooked, &error));
    EXPECT_NE(error.find("\"menu.play\""), std::string::npos) << error;
    EXPECT_NE(error.find("rename"), std::string::npos) << error;
}

TEST_F(LocalizationTest, DeviceLocalesChangingReresolve) {
    // a platform's event (Android configuration, Apple notification, WM_SETTINGCHANGE "intl", the
    // web's languagechange) goes through System::notifyLocalesMayHaveChanged: a new list re-resolves,
    // the same list does nothing
    TestSystem::get().preferredLocales = {"de-DE"};
    install("en", "en", {{"en", {{"play", "Play"}}}, {"de", {{"play", "Spielen"}}}, {"ja", {{"play", "プレイ"}}}});
    TestSystem::get().notifyLocalesMayHaveChanged();
    int events = 0;
    Localization::onLanguageChanged.add("test", [&events](const std::string&) { events++; });
    EXPECT_FALSE(TestSystem::get().notifyLocalesMayHaveChanged()) << "the same languages";
    TestSystem::get().preferredLocales = {"ja-JP", "de-DE"};
    EXPECT_TRUE(TestSystem::get().notifyLocalesMayHaveChanged());
    EXPECT_EQ(Localization::getLanguage(), "ja");
    EXPECT_EQ(events, 1);
}

TEST_F(LocalizationTest, PseudoLocalizeKeepsArgumentsAndSyntax) {
    EXPECT_EQ(Localization::pseudoLocalize("Play", "en-XA"), "\xE2\x9F\xA6\xC3\x9E\xC4\xBA\xC3\xA1\xC3\xBD\xC2\xB7\xC2\xB7\xE2\x9F\xA7");
    EXPECT_EQ(Localization::pseudoLocalize("Play", "ar-XB"), "yalP");
    const std::string withArgument = Localization::pseudoLocalize("Hi {name}", "en-XA");
    EXPECT_NE(withArgument.find("{name}"), std::string::npos) << withArgument;
}

// D-15 (from S95): a key whose first segment names no namespace reads the Default Table, an
// explicit one when two tables exist and none when it is empty; positional arguments are named "0", "1"
TEST_F(LocalizationTest, BareKeysReadTheDefaultTable) {
    install("en", "en", {{"en", {{"play", "Play"}, {"menu.quit", "Quit"}}}});
    LocalizationManifest manifest = Localization::getManifest();
    LocalizationNamespace hud;
    hud.name = "hud";
    hud.sourceLanguage = "en";
    hud.files["en"] = "hud.en.mem";
    memoryTables()["hud.en.mem"] = {{"play", "Hud play"}, {"lives", "{0} of {1}"}};
    manifest.namespaces.push_back(hud);
    // two tables, no Default Table: a bare key is unknown
    Localization::setManifest(manifest);
    EXPECT_EQ(Localization::get("ui.play"), "Play");
    EXPECT_FALSE(Localization::has("play"));
    // the Default Table names one; a key with dots that is not a namespace still reads it
    manifest.defaultTable = "hud";
    Localization::setManifest(manifest);
    EXPECT_EQ(Localization::get("play"), "Hud play");
    EXPECT_EQ(Localization::get("ui.play"), "Play");
    EXPECT_EQ(Localization::get("lives", {{"0", int64_t(2)}, {"1", int64_t(3)}}), "2 of 3");
    manifest.defaultTable = "ui";
    Localization::setManifest(manifest);
    EXPECT_EQ(Localization::get("menu.quit"), "Quit");
    LocalizationManifest reread;
    std::string error;
    ASSERT_TRUE(LocalizationManifest::fromJson(manifest.toJson(), reread, &error)) << error;
    EXPECT_EQ(reread.defaultTable, "ui");
    // one table and no Default Table: that table
    install("en", "en", {{"en", {{"play", "Play"}}}});
    EXPECT_EQ(Localization::get("play"), "Play");
}

// R13, R16, D-15 (from S95): a keyed Text in a scene that is not running (the editor's Edit mode)
// shows its message, follows a table edit at once, and keeps the authored text it saves
TEST_F(LocalizationTest, KeyedTextShowsItsMessageInEditModeAndKeepsTheAuthoredText) {
    install("en", "en", {{"en", {{"title", "Hello world"}}}});
    Scene scene;
    Entity camera = scene.createEntity();
    scene.addComponent<Transform>(camera, Transform());
    CameraComponent cameraComponent;
    cameraComponent.type = CameraType::CAMERA_UI;
    scene.addComponent<CameraComponent>(camera, cameraComponent);
    scene.setCamera(camera);
    Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, Transform());
    scene.addComponent<RectTransformComponent>(entity, RectTransformComponent());
    scene.addComponent<UIComponent>(entity, UIComponent());
    TextComponent component;
    component.font[0] = pixelart::kBuiltinPixelFont;
    component.text = "X";
    component.fontSize = 9;
    scene.addComponent<TextComponent>(entity, component);
    ASSERT_FALSE(Engine::isSceneRunning(&scene));
    UISystem& ui = *scene.getSystem<UISystem>();
    auto vertices = [&]() {
        Attribute* attribute = scene.getComponent<UIComponent>(entity).buffer.getAttribute(AttributeType::POSITION);
        return attribute ? attribute->getCount() : 0u;
    };
    ui.update(0.0);
    const unsigned int authored = vertices();
    ASSERT_GT(authored, 0u);

    scene.getComponent<TextComponent>(entity).localizationKey = "ui.title";
    scene.getComponent<TextComponent>(entity).needUpdateText = true;
    ui.update(0.0);
    const unsigned int shown = vertices();
    EXPECT_GT(shown, authored * 5) << "the message's 10 glyphs, not the authored text's one";
    EXPECT_EQ(scene.getComponent<TextComponent>(entity).text, "X") << "Edit mode keeps the text a save writes";

    // a table edit reaches it at once
    install("en", "en", {{"en", {{"title", "Hi"}}}});
    ui.update(0.0);
    EXPECT_LT(vertices(), shown);
    EXPECT_GT(vertices(), authored);

    // clearing the key shows the authored text again
    scene.getComponent<TextComponent>(entity).localizationKey.clear();
    scene.getComponent<TextComponent>(entity).needUpdateText = true;
    ui.update(0.0);
    EXPECT_EQ(vertices(), authored);
}

}  // namespace LocalizationTest_cpp
