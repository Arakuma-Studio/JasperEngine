// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Editing a title in one of its languages (docs/specs/S87-title-localisation-interchange.md R52):
// an edit of an overridable member goes to the language's Translation (size as size_scale, the
// position as an offset), a shared member edits the document, the source is unchanged by a
// language edit, and Apply to Source copies an override into the layer.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "titleart/localisation/TitleLanguageEditing.h"
#include "titleart/localisation/TitleLanguageOps.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using TitleLanguageEditingJson = titleart::Json;   // its own name: editor::Json (McpJson.h) may be in sight in a unity unit

namespace {

    titleart::TitleArtDoc titleLanguageEditingSample() {
        registerTitleLanguageOps();
        TitleLanguageEditingJson root = TitleLanguageEditingJson{{"format", titleart::TITLE_ART_FORMAT}, {"version", 1}};
        root["layers"] = TitleLanguageEditingJson::array({TitleLanguageEditingJson{{"id", "0000000a"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 100},
                                           {"transform", TitleLanguageEditingJson{{"position", TitleLanguageEditingJson::array({10, 20})}}}}});
        titleart::TitleArtDoc doc = titleart::parseTitleArtJson(root).doc;
        return TitleArtOps::apply(doc, TitleLanguageEditingJson{{"op", "add_language"}, {"code", "de"}, {"source", "en"}}).doc;
    }

}

TEST(TitleLanguageEditing, OverridablesGoToTheTranslation) {
    const titleart::TitleArtDoc doc = titleLanguageEditingSample();
    const TitleLanguageEditingJson routed = routeTitleLanguageOps(doc, "de", TitleLanguageEditingJson::array({TitleLanguageEditingJson{{"op", "set"}, {"layer", "Title"},
        {"values", TitleLanguageEditingJson{{"text", "GEWINN"}, {"size", 80}, {"transform.position", TitleLanguageEditingJson::array({15, 20})}, {"face.fill.color", "#ff0000"}}}}}));
    ASSERT_EQ(routed.size(), 2u);
    EXPECT_EQ(routed[0]["op"], "set") << "the colour is shared";
    EXPECT_TRUE(routed[0]["values"].contains("face.fill.color"));
    EXPECT_EQ(routed[1]["op"], "set_translation");
    EXPECT_EQ(routed[1]["text"], "GEWINN");
    EXPECT_DOUBLE_EQ(routed[1]["values"]["size_scale"].get<double>(), 0.8);
    EXPECT_EQ(routed[1]["values"]["offset"], TitleLanguageEditingJson::array({5.0, 0.0}));
    const TitleOpsResult r = TitleArtOps::apply(doc, routed);
    ASSERT_TRUE(r.ok) << r.error();
    const TitleLanguageEditingJson& layer = *titleart::findLayer(r.doc.root, "0000000a");
    EXPECT_EQ(layer["text"], "WIN") << "the source text stays";
    EXPECT_DOUBLE_EQ(layer["size"].get<double>(), 100.0) << "and its size";
    const titleart::TitleArtDoc resolved = titleart::resolveTitleLanguage(r.doc, "de");
    const TitleLanguageEditingJson& german = *titleart::findLayer(resolved.root, "0000000a");
    EXPECT_DOUBLE_EQ(german["size"].get<double>(), 80.0);
    EXPECT_EQ(german["transform"]["position"], TitleLanguageEditingJson::array({15.0, 20.0}));
    // the source language and set_text
    EXPECT_EQ(routeTitleLanguageOps(doc, "", TitleLanguageEditingJson{{"op", "set_text"}, {"layer", "Title"}, {"text", "X"}}),
              (TitleLanguageEditingJson{{"op", "set_text"}, {"layer", "Title"}, {"text", "X"}}));
    EXPECT_EQ(routeTitleLanguageOps(doc, "de", TitleLanguageEditingJson{{"op", "set_text"}, {"layer", "Title"}, {"text", "X"}})["op"], "set_translation");
    EXPECT_FALSE(titleFieldIsOverridable("face.fill.color"));
    EXPECT_TRUE(titleFieldIsOverridable("fit.mode"));
    EXPECT_TRUE(titleFieldIsOverridable("chars.2.rotation"));
}

TEST(TitleLanguageEditing, ApplyToSource) {
    titleart::TitleArtDoc doc = titleLanguageEditingSample();
    doc = TitleArtOps::apply(doc, TitleLanguageEditingJson{{"op", "set_translation"}, {"layer", "Title"}, {"language", "de"}, {"text", "GEWINN"},
                                       {"values", TitleLanguageEditingJson{{"size_scale", 0.5}}}}).doc;
    EXPECT_EQ(titleLayerOverrides(*titleart::findLayer(doc.root, "0000000a"), "de"), std::vector<std::string>({"size_scale"}));
    const TitleLanguageEditingJson ops = titleApplyToSourceOp(doc, "0000000a", "de", "size_scale");
    const TitleOpsResult r = TitleArtOps::apply(doc, ops);
    ASSERT_TRUE(r.ok) << r.error();
    const TitleLanguageEditingJson& layer = *titleart::findLayer(r.doc.root, "0000000a");
    EXPECT_DOUBLE_EQ(layer["size"].get<double>(), 50.0) << "the source takes the German size";
    EXPECT_FALSE(layer["translations"]["de"].contains("size_scale")) << "and the override is gone";
}
