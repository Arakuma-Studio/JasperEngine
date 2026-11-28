// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Keyed HTML UI (docs/specs/game-ui-localization.md R11, R12, R20, AC7, AC12): data-l10n-id and
// data-l10n-attr-* translated on load and again in place on a language change, the root's
// language and direction, inserted markup translated as it is made, elements with children left
// as written, documents without keys untouched, and the literal-text scan.

#include "TestDocumentCommon.h"

#include "htmlui/HtmlL10n.h"
#include "localization/Localization.h"
#include "localization/StringTableRuntime.h"
#include "util/TitleArtLanguage.h"
#include "component/UIDocumentComponent.h"

#include <RmlUi/Core/Element.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    const char* kMenu =
        "<html>\n<head><title>menu</title></head>\n<body>\n"
        "  <button id=\"play\" data-l10n-id=\"ui.menu.play\">Play</button>\n"
        "  <p id=\"coins\" data-l10n-id=\"ui.shop.coins\" data-l10n-args='{\"count\": 3}'>3 coins</p>\n"
        "  <input id=\"search\" type=\"text\" data-l10n-attr-placeholder=\"ui.search.hint\" placeholder=\"Search\"/>\n"
        "  <p id=\"missing\" data-l10n-id=\"ui.nothing.here\">Authored</p>\n"
        "  <div id=\"nested\" data-l10n-id=\"ui.menu.play\"><span>child</span></div>\n"
        "  <p id=\"brand\" translate=\"no\">JasperQuest</p>\n"
        "  <p id=\"options\">Options</p>\n"
        "  <div id=\"list\"></div>\n"
        "</body>\n</html>\n";

    class HtmlL10nTest : public ::testing::Test {
    protected:
        fs::path dir;

        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            Localization::setEditorMode(true);
            Localization::registerReader(".mem", [](const std::string& path, const std::string& language, std::string*) {
                std::vector<l10n::StringTable::Entry> entries;
                if (path == "ui.en.mem") {
                    entries = {{"menu.play", "Play"}, {"shop.coins", "{count, plural, one {# coin} other {# coins}}"}, {"search.hint", "Search"}};
                } else if (path == "ui.de.mem") {
                    entries = {{"menu.play", "Spielen"}, {"shop.coins", "{count, plural, one {# Münze} other {# Münzen}}"}, {"search.hint", "Suchen"}};
                } else if (path == "ui.ar.mem") {
                    entries = {{"menu.play", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA8"}};
                }
                return l10n::StringTable::fromEntries(language, entries);
            });
            LocalizationManifest manifest;
            manifest.defaultLanguage = "en";
            manifest.rememberLanguage = false;
            manifest.languages = {"en", "de", "ar"};
            manifest.namespaces.push_back({"ui", "en", {{"en", "ui.en.mem"}, {"de", "ui.de.mem"}, {"ar", "ui.ar.mem"}}});
            Localization::setManifest(manifest);
            ASSERT_TRUE(Localization::setLanguage("de"));
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "l10n" / info->name();
            fs::remove_all(dir);
            fs::create_directories(dir);
            std::ofstream(dir / "menu.rml", std::ios::binary) << kMenu;
        }

        void TearDown() override{
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
        }

        std::string path(const std::string& name) const{ return (dir / name).generic_string(); }
    };

    std::string textOf(Rml::Element* element){
        return element ? element->GetInnerRML() : std::string("(none)");
    }

}

TEST_F(HtmlL10nTest, TranslatesKeyedElementsAndAttributes){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(textOf(doc->GetElementById("play")), "Spielen");
    EXPECT_EQ(textOf(doc->GetElementById("coins")), "3 Münzen");
    EXPECT_EQ(doc->GetElementById("search")->GetAttribute<Rml::String>("placeholder", ""), "Suchen");
    // a key no table has keeps the authored text
    EXPECT_EQ(textOf(doc->GetElementById("missing")), "Authored");
}

TEST_F(HtmlL10nTest, SwitchesLanguageWithoutReload){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    Rml::Element* play = doc->GetElementById("play");
    ASSERT_TRUE(Localization::setLanguage("en"));
    EXPECT_EQ(doc->GetElementById("play"), play) << "the same element, translated in place";
    EXPECT_EQ(textOf(play), "Play");
    EXPECT_EQ(textOf(doc->GetElementById("coins")), "3 coins");
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 1u);
}

TEST_F(HtmlL10nTest, SetsLangAndDirOnTheRoot){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->GetProperty("-rmlui-language")->ToString(), "de");
    ASSERT_TRUE(Localization::setLanguage("ar"));
    EXPECT_EQ(doc->GetProperty("-rmlui-language")->ToString(), "ar");
    EXPECT_EQ(doc->GetProperty("-rmlui-direction")->ToString(), "rtl");
    EXPECT_EQ(textOf(doc->GetElementById("play")), "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA8");
}

TEST_F(HtmlL10nTest, TemplateRowsAndInsertedHtmlAreTranslated){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    // markup a script inserts (innerHTML): translated in the frame it appears
    Rml::Element* list = doc->GetElementById("list");
    std::string rows;
    for (int i = 0; i < 1000; i++) rows += "<p data-l10n-id=\"ui.menu.play\">Play</p>";
    list->SetInnerRML(rows);
    htmlui::HtmlL10n::update();
    ASSERT_EQ(list->GetNumChildren(), 1000);
    EXPECT_EQ(textOf(list->GetChild(0)), "Spielen");
    EXPECT_EQ(textOf(list->GetChild(999)), "Spielen");
}

TEST_F(HtmlL10nTest, ChildElementsReportL10n0001){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    TestSystem::get().clearLogs();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(textOf(doc->GetElementById("nested")), "<span>child</span>") << "left as written";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-LOC-0001"), 1u);
}

TEST_F(HtmlL10nTest, DocumentsWithoutL10nAreUntouched){
    std::ofstream(dir / "plain.rml", std::ios::binary) << "<html><head><title>plain</title></head><body><p id=\"a\">Hello</p></body></html>\n";
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("plain.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(htmlui::HtmlL10n::report(doc).keyed, 0u);
    EXPECT_FALSE(doc->GetLocalProperty("-rmlui-language")) << "the root of a document without keys keeps its language";
}

TEST_F(HtmlL10nTest, ScanReportsLiteralTextOnly){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("menu.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* doc = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(doc, nullptr);
    // a text a script set from Localization.get counts as localised
    doc->GetElementById("list")->SetInnerRML(Localization::get("ui.menu.play"));
    const htmlui::HtmlL10n::DocumentReport report = htmlui::HtmlL10n::report(doc);
    std::vector<std::string> literal;
    for (const auto& item : report.literal) literal.push_back(item.second);
    EXPECT_NE(std::find(literal.begin(), literal.end(), "Options"), literal.end());
    EXPECT_EQ(std::find(literal.begin(), literal.end(), "JasperQuest"), literal.end()) << "translate=\"no\"";
    EXPECT_EQ(std::find(literal.begin(), literal.end(), "Spielen"), literal.end()) << "keyed or produced";
    EXPECT_EQ(std::find(literal.begin(), literal.end(), "Authored"), literal.end()) << "under a keyed element";
    EXPECT_GE(report.keyed, 4u);
}
