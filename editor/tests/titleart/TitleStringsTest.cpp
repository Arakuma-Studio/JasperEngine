// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Strings exchange (docs/specs/S87-title-localisation-interchange.md R20-R23, AC11, AC12): the
// CSV round trip, BOM / delimiters / line ends / quoted newlines, Unity headers, unknown keys and
// languages, source changes, empty cells, multi-document routing; XLIFF 1.2 export structure,
// 1.2 and 2.0 import with the state mapping and inline markup refused per unit.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "titleart/localisation/TitleLanguageOps.h"
#include "titleart/localisation/TitleStrings.h"
#include "tinyxml2.h"

#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;
namespace fs = std::filesystem;

namespace {

    class TitleStringsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerTitleLanguageOps();
            registerTitleStringsOps();
            dir = fs::temp_directory_path() / "je_title_strings";
            fs::create_directories(dir / "assets" / "titles");
            options.projectPath = dir.string();
            options.documentPath = (dir / "assets" / "titles" / "logo.titleart").string();
            Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", 1}};
            root["layers"] = Json::array({Json{{"id", "0000000a"}, {"type", "text"}, {"name", "Headline"}, {"text", "BIG WIN"},
                                               {"translator_note", "Main title, max one line"}, {"max_chars", 12}},
                                          Json{{"id", "0000000b"}, {"type", "text"}, {"name", "Sub"}, {"text", "a \"quoted\", line\nand more"}}});
            doc = titleart::parseTitleArtJson(root).doc;
            doc = TitleArtOps::apply(doc, Json::array({Json{{"op", "add_language"}, {"code", "de"}, {"source", "en"}},
                                                       Json{{"op", "add_language"}, {"code", "ja"}},
                                                       Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "de"}, {"text", "GROSSER GEWINN"}},
                                                       Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "ja"},
                                                            {"text", "\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a"}}}),
                                     options).doc;
        }
        void TearDown() override { fs::remove_all(dir); }

        std::string write(const std::string& name, const std::string& text) {
            const fs::path p = dir / name;
            std::ofstream(p, std::ios::binary) << text;
            return p.string();
        }

        TitleOpsResult import(const std::string& file, Json extra = Json::object()) {
            Json op = Json{{"op", "import_strings"}, {"file", file}};
            for (auto it = extra.begin(); it != extra.end(); ++it) op[it.key()] = it.value();
            return TitleArtOps::apply(doc, op, options);
        }

        const Json& layer(const TitleArtDoc& d, const char* id) { return *titleart::findLayer(d.root, id); }

        fs::path dir;
        TitleArtDoc doc;
        TitleOpsOptions options;
    };

}

TEST_F(TitleStringsTest, CsvExportShape) {
    const std::string csv = exportTitleStringsCsv({{"assets/titles/logo.titleart", &doc}}, {});
    ASSERT_EQ(csv.rfind("\xEF\xBB\xBF", 0), 0u) << "UTF-8 with BOM";
    const std::string body = csv.substr(3);
    EXPECT_EQ(body.substr(0, body.find("\r\n")), "key,context,max_chars,en,de,ja");
    EXPECT_NE(body.find("logo.headline,\"Main title, max one line\",12,BIG WIN,GROSSER GEWINN,\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\r\n"), std::string::npos);
    EXPECT_NE(body.find("\"a \"\"quoted\"\", line\nand more\""), std::string::npos) << "quotes doubled, the newline inside the quotes";
    // several documents: a file column first
    const std::string multi = exportTitleStringsCsv({{"assets/titles/logo.titleart", &doc}, {"assets/titles/other.titleart", &doc}}, {"de"});
    EXPECT_EQ(multi.substr(3, multi.find("\r\n") - 3), "file,key,context,max_chars,en,de");
}

TEST_F(TitleStringsTest, CsvRoundTripIsAnIdentity) {
    const std::string csv = exportTitleStringsCsv({{"assets/titles/logo.titleart", &doc}}, {});
    const TitleOpsResult r = import(write("strings.csv", csv));
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(titleart::writeTitleArt(r.doc), titleart::writeTitleArt(doc)) << "nothing changes";
    EXPECT_EQ(r.reports["0"]["applied"], 0);
    EXPECT_EQ(r.reports["0"]["unchanged"], 2);
}

TEST_F(TitleStringsTest, CsvDialects) {
    for (const std::string& csv : {
             std::string("key;en;German (de)\nlogo.sub;\"a \"\"quoted\"\", line\nand more\";\"Zeile;\nzwei\"\n"),     // ; LF, a Unity header
             std::string("\xEF\xBB\xBFkey\ten\tde\r\nlogo.sub\t\"a \"\"quoted\"\", line\nand more\"\t\"Zeile;\nzwei\"\r\n"),   // BOM, tab, CRLF
         }) {
        const TitleOpsResult r = import(write("dialect.csv", csv));
        ASSERT_TRUE(r.ok) << r.error();
        EXPECT_EQ(layer(r.doc, "0000000b")["translations"]["de"]["text"], "Zeile;\nzwei");
        EXPECT_EQ(layer(r.doc, "0000000b")["translations"]["de"]["status"], "translated");
    }
}

TEST_F(TitleStringsTest, CsvReports) {
    const std::string csv = "key,en,de,fr\r\n"
                            "logo.headline,BIG WIN,,GROS GAIN\r\n"                  // empty cell: nothing changes
                            "logo.sub,a different source,NEU,\r\n"                   // the source changed
                            "logo.missing,x,y,z\r\n";                                // unknown key
    TitleOpsResult r = import(write("report.csv", csv));
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["de"]["text"], "GROSSER GEWINN");
    EXPECT_EQ(layer(r.doc, "0000000b")["translations"]["de"]["status"], "needs_review");
    std::set<std::string> codes;
    for (const Json& n : r.reports["0"]["notes"]) codes.insert(n["code"].get<std::string>());
    EXPECT_TRUE(codes.count("unknown_key"));
    EXPECT_TRUE(codes.count("unknown_language")) << "fr is not a language of the title";
    EXPECT_TRUE(codes.count("source_changed"));
    // add_languages adds fr
    r = import(write("report.csv", csv), Json{{"add_languages", true}});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(titleart::findTitleLanguage(r.doc.root, "fr"));
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["fr"]["text"], "GROS GAIN");
}

TEST_F(TitleStringsTest, CsvRoutesByFile) {
    const std::string csv = "file,key,en,de\r\nassets/titles/other.titleart,logo.headline,BIG WIN,ANDERS\r\nassets/titles/logo.titleart,logo.headline,BIG WIN,DIESER\r\n";
    const TitleOpsResult r = import(write("route.csv", csv));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["de"]["text"], "DIESER");
    EXPECT_EQ(r.reports["0"]["rows"], 1);
}

TEST_F(TitleStringsTest, XliffExportStructure) {
    const auto files = exportTitleStringsXliff({{"assets/titles/logo.titleart", &doc}}, {"de"});
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].first, "de");
    tinyxml2::XMLDocument xml;
    ASSERT_EQ(xml.Parse(files[0].second.c_str()), tinyxml2::XML_SUCCESS);
    const tinyxml2::XMLElement* root = xml.RootElement();
    EXPECT_STREQ(root->Attribute("version"), "1.2");
    const tinyxml2::XMLElement* file = root->FirstChildElement("file");
    ASSERT_TRUE(file);
    EXPECT_STREQ(file->Attribute("original"), "assets/titles/logo.titleart");
    EXPECT_STREQ(file->Attribute("source-language"), "en");
    EXPECT_STREQ(file->Attribute("target-language"), "de");
    const tinyxml2::XMLElement* unit = file->FirstChildElement("body")->FirstChildElement("trans-unit");
    ASSERT_TRUE(unit);
    EXPECT_STREQ(unit->Attribute("id"), "logo.headline");
    EXPECT_STREQ(unit->Attribute("maxwidth"), "12");
    EXPECT_STREQ(unit->Attribute("size-unit"), "char");
    EXPECT_STREQ(unit->FirstChildElement("source")->GetText(), "BIG WIN");
    EXPECT_STREQ(unit->FirstChildElement("target")->GetText(), "GROSSER GEWINN");
    EXPECT_STREQ(unit->FirstChildElement("target")->Attribute("state"), "translated");
    EXPECT_STREQ(unit->FirstChildElement("note")->GetText(), "Main title, max one line");
    // and back
    const TitleOpsResult r = import(write("logo.de.xlf", files[0].second));
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(titleart::writeTitleArt(r.doc), titleart::writeTitleArt(doc));
}

TEST_F(TitleStringsTest, XliffImportStates) {
    const std::string v12 = R"(<?xml version="1.0"?><xliff version="1.2"><file original="assets/titles/logo.titleart" source-language="en" target-language="de"><body>
        <trans-unit id="logo.headline"><source>BIG WIN</source><target state="needs-review-translation">GEWINN</target></trans-unit>
        <trans-unit id="logo.sub"><source>x</source><target state="final">mit <g id="1">Markup</g></target></trans-unit>
        </body></file></xliff>)";
    TitleOpsResult r = import(write("v12.xlf", v12));
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["de"]["status"], "needs_review");
    bool refused = false;
    for (const Json& n : r.reports["0"]["notes"]) refused = refused || n["code"] == "inline_markup";
    EXPECT_TRUE(refused) << "a unit with inline markup is refused";
    EXPECT_FALSE(layer(r.doc, "0000000b")["translations"].contains("de"));

    const std::string v20 = R"(<xliff version="2.0" srcLang="en" trgLang="ja" xmlns="urn:oasis:names:tc:xliff:document:2.0"><file id="f1" original="assets/titles/logo.titleart">
        <unit id="logo.headline"><segment state="reviewed"><source>BIG WIN</source><target>大勝利</target></segment></unit>
        <unit id="logo.sub"><segment state="initial"><source>a "quoted", line
and more</source><target>二行</target></segment></unit>
        </file></xliff>)";
    r = import(write("v20.xlf", v20));
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["ja"]["text"], "\xe5\xa4\xa7\xe5\x8b\x9d\xe5\x88\xa9");
    EXPECT_EQ(layer(r.doc, "0000000a")["translations"]["ja"]["status"], "translated");
    EXPECT_EQ(layer(r.doc, "0000000b")["translations"]["ja"]["status"], "needs_review");
}

TEST_F(TitleStringsTest, MalformedFilesFailWithAMessage) {
    std::string error;
    TitleStringsFile out;
    EXPECT_FALSE(parseTitleStringsCsv("key,en\n\"unclosed,x\n", out, error));
    EXPECT_NE(error.find("not closed"), std::string::npos);
    EXPECT_FALSE(parseTitleStringsCsv("en,de\n", out, error)) << "no key column";
    EXPECT_FALSE(parseTitleStringsXliff("<xliff version=\"3.0\"/>", out, error));
    EXPECT_FALSE(parseTitleStringsXliff("<notxliff/>", out, error));
    EXPECT_FALSE(parseTitleStringsXliff("<xliff", out, error));
}
