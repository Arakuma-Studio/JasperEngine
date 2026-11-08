// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A title in several languages (docs/specs/S87-title-localisation-interchange.md R1-R7, R16, R50,
// D2, D5, AC3): per-member inheritance from the layer, the Language font, size_scale, the shaping
// language, only_languages, exposed text overrides in every language, sparse Translations and the
// document rules normalize applies.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleTranslations.h"

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    TitleArtDoc multilingual() {
        Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"source_language", "en"},
                         {"languages", Json::array({Json{{"code", "de"}}, Json{{"code", "ja"}, {"font", fontRef(DELA_GOTHIC)}}})}};
        root["layers"] = Json::array({
            Json{{"id", "0000000a"}, {"type", "text"}, {"name", "Title"}, {"text", "BIG WIN"}, {"size", 120}, {"font", fontRef(OSWALD)},
                 {"tracking", 5}, {"translations", Json{{"de", Json{{"text", "GROSSER GEWINN"}, {"size_scale", 0.5}, {"tracking", 2}}},
                                                         {"ja", Json{{"text", "\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a"}, {"vertical", true}}}}}},
            Json{{"id", "0000000b"}, {"type", "text"}, {"name", "Reading"}, {"text", "\xe3\x81\x9f\xe3\x81\x84"}, {"only_languages", Json::array({"ja"})}},
            Json{{"id", "0000000c"}, {"type", "text"}, {"name", "Brand"}, {"text", "ACME"}, {"translate", false}},
        });
        root["exposed"] = Json::array({Json{{"name", "text"}, {"type", "text"}, {"targets", Json::array({Json{{"layer", "0000000a"}, {"field", "text"}}})}}});
        return parseTitleArtJson(root).doc;
    }

    const Json& layer(const TitleArtDoc& doc, const char* id) {
        return *findLayer(doc.root, id);
    }

}

TEST(TitleTranslationsTest, MembersInheritAndOverride) {
    const TitleArtDoc doc = multilingual();
    const TitleArtDoc de = resolveTitleLanguage(doc, "de");
    const Json& title = layer(de, "0000000a");
    EXPECT_EQ(title["text"], "GROSSER GEWINN");
    EXPECT_DOUBLE_EQ(title["size"].get<double>(), 60.0) << "size x size_scale";
    EXPECT_DOUBLE_EQ(title["tracking"].get<double>(), 2.0) << "a present override wins";
    EXPECT_EQ(title["font"]["path"], OSWALD) << "an absent override inherits the layer's member";
    EXPECT_EQ(title["language"], "de") << "the shaping language is the code";
    EXPECT_FALSE(layer(de, "0000000b")["visible"].get<bool>()) << "only_languages hides it outside Japanese";

    const TitleArtDoc ja = resolveTitleLanguage(doc, "ja");
    EXPECT_EQ(layer(ja, "0000000a")["font"]["path"], DELA_GOTHIC) << "the Language's font when the Translation has none";
    EXPECT_TRUE(layer(ja, "0000000a")["vertical"].get<bool>());
    EXPECT_TRUE(layer(ja, "0000000b")["visible"].get<bool>());
    EXPECT_EQ(layer(ja, "0000000c")["font"].is_null(), true) << "a brand word (translate false) keeps its own font";
    EXPECT_EQ(layer(ja, "0000000c")["text"], "ACME");

    const TitleArtDoc en = resolveTitleLanguage(doc, "");
    EXPECT_EQ(layer(en, "0000000a")["text"], "BIG WIN");
    EXPECT_EQ(layer(en, "0000000a")["language"], "en") << "the source shapes with source_language";
    EXPECT_FALSE(layer(en, "0000000b")["visible"].get<bool>()) << "the source is hidden unless listed";

    // resolving a resolved copy changes nothing
    EXPECT_EQ(writeTitleArt(resolveTitleLanguage(de, "de")), writeTitleArt(de));
    EXPECT_EQ(writeTitleArt(resolveTitleLanguage(de, "")), writeTitleArt(de));
}

TEST(TitleTranslationsTest, ExposedTextOverrideWinsInEveryLanguage) {
    const TitleArtDoc doc = multilingual();
    for (const char* code : {"", "de", "ja"}) {
        TitleArtDoc resolved = resolveTitleLanguage(doc, code);
        applyFieldOverrides(resolved, {FieldOverride{"0000000a", "text", "SCORE 100"}});
        EXPECT_EQ(layer(resolved, "0000000a")["text"], "SCORE 100") << code;
    }
    // the language still supplies the layout
    TitleArtDoc ja = resolveTitleLanguage(doc, "ja");
    applyFieldOverrides(ja, {FieldOverride{"0000000a", "text", "SCORE 100"}});
    EXPECT_TRUE(layer(ja, "0000000a")["vertical"].get<bool>());
}

TEST(TitleTranslationsTest, SparseTranslationsFollowTheSource) {
    TitleArtDoc doc = multilingual();
    // the writer emits text, status, source_hash, then only the present overrides, in table order
    const Json& de = layer(doc, "0000000a")["translations"]["de"];
    std::vector<std::string> keys;
    for (auto it = de.begin(); it != de.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, std::vector<std::string>({"text", "status", "source_hash", "size_scale", "tracking"}));
    // a later source edit reaches the language that does not override it
    findLayer(doc.root, "0000000a")->operator[]("line_height") = 2.0;
    normalizeTitleArt(doc);
    EXPECT_DOUBLE_EQ(layer(resolveTitleLanguage(doc, "de"), "0000000a")["line_height"].get<double>(), 2.0);
    // and survives a save
    EXPECT_EQ(writeTitleArt(parseTitleArt(writeTitleArt(doc)).doc), writeTitleArt(doc));
}

// AC3's red control: a Translation that wrote every member would freeze today's source values
TEST(TitleTranslationsTest, AFullTranslationWouldFreezeTheSource) {
    TitleArtDoc doc = multilingual();
    Json& de = (*findLayer(doc.root, "0000000a"))["translations"]["de"];
    de["line_height"] = 1.1;    // what a non-sparse writer would have stored
    findLayer(doc.root, "0000000a")->operator[]("line_height") = 2.0;
    normalizeTitleArt(doc);
    EXPECT_DOUBLE_EQ(layer(resolveTitleLanguage(doc, "de"), "0000000a")["line_height"].get<double>(), 1.1)
        << "the frozen value no longer follows the source: why Translations are sparse";
}

TEST(TitleTranslationsTest, NormalizeRules) {
    Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"source_language", "EN"},
                     {"languages", Json::array({Json{{"code", "pt_br"}}, Json{{"code", "PT-BR"}}, Json{{"code", "en"}}, Json{{"code", "x!"}}, Json{{"code", "de"}}})},
                     {"outputs", Json::array({Json{{"id", "main"}}, Json{{"id", "de"}}})}};
    root["layers"] = Json::array({Json{{"id", "0000000a"}, {"type", "text"}, {"text", "A"}, {"text_key", "title"},
                                       {"translations", Json{{"pt-br", Json{{"text", "B"}}}}}},
                                  Json{{"id", "0000000b"}, {"type", "text"}, {"text", "C"}, {"text_key", "title"}}});
    std::vector<TitleWarning> warnings;
    TitleArtParseResult parsed = parseTitleArtJson(root);
    const TitleArtDoc& doc = parsed.doc;
    EXPECT_EQ(doc.root["source_language"], "en");
    ASSERT_EQ(doc.root["languages"].size(), 2u) << "duplicates, the source and an invalid tag are dropped";
    EXPECT_EQ(doc.root["languages"][0]["code"], "pt-BR");
    EXPECT_TRUE(doc.layers()[0]["translations"].contains("pt-BR")) << "translation keys in canonical case";
    EXPECT_EQ(doc.layers()[1]["text_key"], "title_2") << "text keys are unique";
    EXPECT_EQ(doc.root["outputs"][1]["id"], "de_2") << "output_id_is_language";
    bool reported = false;
    for (const TitleWarning& w : parsed.warnings) reported = reported || w.message.find("output_id_is_language") != std::string::npos;
    EXPECT_TRUE(reported);
}

TEST(TitleTranslationsTest, MonolingualIsUntouched) {
    const TitleArtDoc doc = documentWith({Json{{"id", "0000000a"}, {"type", "text"}, {"text", "Hi"}, {"only_languages", Json::array({"ja"})}}});
    EXPECT_FALSE(titleIsMultilingual(doc.root));
    EXPECT_EQ(writeTitleArt(resolveTitleLanguage(doc, "ja")), writeTitleArt(doc)) << "R50: no S87 path runs";
    EXPECT_FALSE(titleLanguageDiffers(doc, "ja"));
}

TEST(TitleTranslationsTest, WhichLanguagesDiffer) {
    TitleArtDoc doc = multilingual();
    EXPECT_TRUE(titleLanguageDiffers(doc, "de"));
    EXPECT_TRUE(titleLanguageDiffers(doc, "ja"));
    // a language with no translation, no font and no only_languages difference reuses the source set
    doc.root["languages"].push_back(Json{{"code", "fr"}});
    findLayer(doc.root, "0000000b")->erase("only_languages");
    normalizeTitleArt(doc);
    EXPECT_FALSE(titleLanguageDiffers(doc, "fr"));
    EXPECT_EQ(translationSourceHash("BIG WIN"), translationSourceHash("BIG WIN"));
    EXPECT_EQ(translationSourceHash("").size(), 8u);
    EXPECT_EQ(translationSourceHash(""), "811c9dc5") << "FNV-1a 32 offset basis";
}
