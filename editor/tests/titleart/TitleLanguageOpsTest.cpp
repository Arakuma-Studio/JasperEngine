// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The language ops (docs/specs/S87-title-localisation-interchange.md R1-R3, R8, D6, §6.2):
// add_language (source, text keys, namespace, the default fit), remove_language,
// set_source_language with and without the swap, set_translation, revert_translation and
// mark_reviewed, each one batch of S80's ops.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "titleart/localisation/TitleLanguageOps.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    class TitleLanguageOpsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerTitleLanguageOps();
            Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", 1}};
            root["layers"] = Json::array({Json{{"id", "0000000a"}, {"type", "text"}, {"name", "Headline"}, {"text", "BIG WIN"}, {"size", 100}},
                                          Json{{"id", "0000000b"}, {"type", "text"}, {"name", "Sub Line"}, {"text", "tonight"}},
                                          Json{{"id", "0000000c"}, {"type", "text"}, {"name", "Brand"}, {"text", "ACME"}, {"translate", false}}});
            doc = titleart::parseTitleArtJson(root).doc;
            options.documentPath = "/project/assets/titles/logo.titleart";
            options.idSeed = 7;
        }

        TitleOpsResult apply(const Json& ops) {
            TitleOpsResult r = TitleArtOps::apply(doc, ops, options);
            if (r.ok) doc = r.doc;
            return r;
        }

        const Json& layer(const char* id) { return *titleart::findLayer(doc.root, id); }

        TitleArtDoc doc;
        TitleOpsOptions options;
    };

}

TEST_F(TitleLanguageOpsTest, AddLanguagePreparesTheTitle) {
    const TitleOpsResult r = apply(Json{{"op", "add_language"}, {"code", "de"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(doc.root["source_language"], "en") << "no source given and no project default: en";
    ASSERT_EQ(doc.root["languages"].size(), 1u);
    EXPECT_EQ(doc.root["languages"][0]["code"], "de");
    EXPECT_EQ(doc.root["string_namespace"], "logo") << "from the file stem";
    EXPECT_EQ(layer("0000000a")["text_key"], "headline");
    EXPECT_EQ(layer("0000000b")["text_key"], "sub_line");
    // R8: a box of the source extent + 10 % and shrink, so a translation fits by default
    EXPECT_EQ(layer("0000000a")["fit"]["mode"], "shrink");
    EXPECT_GT(layer("0000000a")["fit"]["box"][0].get<double>(), 0.0);
    EXPECT_EQ(layer("0000000c")["fit"]["mode"], "none") << "a brand word is not translated";
    EXPECT_EQ(r.undoName, "Add Language German (de)");

    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "add_language"}, {"code", "de"}}, options).ok) << "language_exists";
    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "add_language"}, {"code", "en"}}, options).ok) << "the source";
    const TitleOpsResult bad = TitleArtOps::apply(doc, Json{{"op", "add_language"}, {"code", "xx_!"}}, options);
    ASSERT_FALSE(bad.ok);
    EXPECT_EQ(bad.problems.front().code, "bad_value");
    // the source given, and the project default when none is
    TitleArtDoc fresh = doc;
    fresh.root["source_language"] = "";
    fresh.root["languages"] = Json::array();
    TitleOpsOptions withDefault = options;
    withDefault.defaultLanguage = "ja";
    const TitleOpsResult ja = TitleArtOps::apply(fresh, Json{{"op", "add_language"}, {"code", "de"}}, withDefault);
    ASSERT_TRUE(ja.ok);
    EXPECT_EQ(ja.doc.root["source_language"], "ja");
}

TEST_F(TitleLanguageOpsTest, SetTranslationStampsTheSource) {
    ASSERT_TRUE(apply(Json{{"op", "add_language"}, {"code", "de"}}).ok);
    const TitleOpsResult r = apply(Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "de"}, {"text", "GROSSER GEWINN"},
                                        {"values", Json{{"size_scale", 0.8}, {"fit.mode", "condense"}}}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& t = layer("0000000a")["translations"]["de"];
    EXPECT_EQ(t["text"], "GROSSER GEWINN");
    EXPECT_EQ(t["status"], "translated");
    EXPECT_EQ(t["source_hash"], titleart::translationSourceHash("BIG WIN"));
    EXPECT_DOUBLE_EQ(t["size_scale"].get<double>(), 0.8);
    EXPECT_EQ(t["fit"].size(), 1u) << "a partial fit: only the members given";
    EXPECT_EQ(t["fit"]["mode"], "condense");
    EXPECT_FALSE(t.contains("font")) << "sparse";
    EXPECT_EQ(r.undoName, "Set Translation (Headline: de)");
    // a language the title lacks is refused with the add_language fix
    const TitleOpsResult missing = TitleArtOps::apply(doc, Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "fr"}, {"text", "x"}}, options);
    ASSERT_FALSE(missing.ok);
    EXPECT_EQ(missing.problems.front().code, "language_unknown");
    EXPECT_EQ(missing.problems.front().fix[0]["op"], "add_language");
}

TEST_F(TitleLanguageOpsTest, RevertAndReview) {
    ASSERT_TRUE(apply(Json::array({Json{{"op", "add_language"}, {"code", "de"}},
                                    Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "de"}, {"text", "GEWINN"},
                                         {"values", Json{{"tracking", 4}}}, {"status", "needs_review"}}})).ok);
    ASSERT_TRUE(apply(Json{{"op", "revert_translation"}, {"layer", "Headline"}, {"language", "de"}}).ok);
    EXPECT_FALSE(layer("0000000a")["translations"]["de"].contains("tracking")) << "every override but the text";
    EXPECT_EQ(layer("0000000a")["translations"]["de"]["text"], "GEWINN");
    // the source changes: the translation is stale until reviewed
    ASSERT_TRUE(apply(Json{{"op", "set"}, {"layer", "Headline"}, {"values", Json{{"text", "BIGGER WIN"}}}}).ok);
    ASSERT_TRUE(apply(Json{{"op", "mark_reviewed"}, {"language", "de"}}).ok);
    EXPECT_EQ(layer("0000000a")["translations"]["de"]["status"], "translated");
    EXPECT_EQ(layer("0000000a")["translations"]["de"]["source_hash"], titleart::translationSourceHash("BIGGER WIN"));
    ASSERT_TRUE(apply(Json{{"op", "revert_translation"}, {"layer", "Headline"}, {"language", "de"}, {"fields", Json::array({"all"})}}).ok);
    EXPECT_FALSE(layer("0000000a")["translations"].contains("de"));
}

TEST_F(TitleLanguageOpsTest, RemoveLanguageCountsWhatIsLost) {
    ASSERT_TRUE(apply(Json::array({Json{{"op", "add_language"}, {"code", "de"}},
                                    Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "de"}, {"text", "A"}},
                                    Json{{"op", "set_translation"}, {"layer", "Sub Line"}, {"language", "de"}, {"text", "B"}}})).ok);
    const TitleOpsResult r = apply(Json{{"op", "remove_language"}, {"code", "de"}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.reports["0"]["translations_removed"], 2);
    EXPECT_TRUE(doc.root["languages"].empty());
    EXPECT_FALSE(layer("0000000a")["translations"].contains("de"));
}

TEST_F(TitleLanguageOpsTest, SwapTheSourceLanguage) {
    ASSERT_TRUE(apply(Json::array({Json{{"op", "add_language"}, {"code", "ja"}, {"source", "en"}},
                                    Json{{"op", "set_translation"}, {"layer", "Headline"}, {"language", "ja"}, {"text", "\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a"},
                                         {"values", Json{{"size_scale", 0.5}, {"vertical", true}}}}})).ok);
    const TitleOpsResult r = apply(Json{{"op", "set_source_language"}, {"code", "ja"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(doc.root["source_language"], "ja");
    ASSERT_EQ(doc.root["languages"].size(), 1u);
    EXPECT_EQ(doc.root["languages"][0]["code"], "en");
    EXPECT_EQ(layer("0000000a")["text"], "\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a");
    EXPECT_DOUBLE_EQ(layer("0000000a")["size"].get<double>(), 50.0);
    EXPECT_TRUE(layer("0000000a")["vertical"].get<bool>());
    const Json& en = layer("0000000a")["translations"]["en"];
    EXPECT_EQ(en["text"], "BIG WIN");
    EXPECT_DOUBLE_EQ(en["size_scale"].get<double>(), 2.0) << "the old overrides moved";
    EXPECT_EQ(en["vertical"], false);
    EXPECT_EQ(en["source_hash"], titleart::translationSourceHash("\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a"));
    // the English rendering is what the source was
    const TitleArtDoc english = titleart::resolveTitleLanguage(doc, "en");
    EXPECT_EQ((*titleart::findLayer(english.root, "0000000a"))["text"], "BIG WIN");
    EXPECT_DOUBLE_EQ((*titleart::findLayer(english.root, "0000000a"))["size"].get<double>(), 100.0);
    // swap: false only relabels, refused for a translation language
    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "set_source_language"}, {"code", "en"}, {"swap", false}}, options).ok);
    const TitleOpsResult relabel = TitleArtOps::apply(doc, Json{{"op", "set_source_language"}, {"code", "ja-JP"}, {"swap", false}}, options);
    ASSERT_TRUE(relabel.ok);
    EXPECT_EQ(relabel.doc.root["source_language"], "ja-JP");
}
