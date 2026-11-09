// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's Add palette entries (docs/specs/S82-title-designer.md R17, D6, AC38;
// S83 §6.10, R41, R42, AC19): the Add Effect categories in §6.10's order with each one's own
// order, recipes and style settings among the effects, generators always layer rows, the
// "Generators that follow this layer" section following the target, English and Japanese search,
// cost classes on the rows; and the entries' ops applied by TitleArtOps give one step each.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/designer/TitleAddCatalog.h"
#include "titleart/fx/FxRecipes.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    AddTarget textTarget(bool is3D = false) {
        AddTarget target;
        target.layers = {"t1"};
        target.name = "Title";
        target.hasStyle = true;
        target.is3D = is3D;
        return target;
    }

    const AddEntry* find(const std::vector<AddEntry>& entries, const std::string& id) {
        for (const AddEntry& e : entries) {
            if (e.id == id) return &e;
        }
        return nullptr;
    }

    std::vector<std::string> matching(const std::vector<AddEntry>& entries, const std::string& query) {
        std::vector<std::string> out;
        for (const AddEntry& e : entries) {
            if (addEntryMatches(e, query)) out.push_back(e.id);
        }
        return out;
    }

    size_t indexOf(const std::vector<AddEntry>& entries, const std::string& id) {
        for (size_t i = 0; i < entries.size(); i++) {
            if (entries[i].id == id) return i;
        }
        return entries.size();
    }

}

// §6.10: the Effects categories in order (then the following generators), each category's entries
// in the table's order. Red control: without the sort, registry order puts S83's Light Glow after
// S80's Blur and the category sequence repeats.
TEST(TitleAddCatalogTest, EffectCategoriesInTheTablesOrder) {
    const std::vector<AddEntry> entries = buildAddEntries("effects", textTarget());
    std::vector<std::string> sequence;
    for (const AddEntry& e : entries) {
        if (e.section != AddEntry::Section::Effects) continue;
        if (sequence.empty() || sequence.back() != e.category) sequence.push_back(e.category);
    }
    std::vector<std::string> expected = addEffectCategories();
    expected.push_back(FOLLOW_CATEGORY);
    EXPECT_EQ(sequence, expected);
    // inside categories: §6.10's rows
    EXPECT_LT(indexOf(entries, "fx:outer_glow"), indexOf(entries, "fx:inner_glow"));
    EXPECT_LT(indexOf(entries, "fx:inner_glow"), indexOf(entries, "fx:light_glow"));
    EXPECT_LT(indexOf(entries, "fx:inner_shadow"), indexOf(entries, "recipe:long_shadow"));
    EXPECT_LT(indexOf(entries, "recipe:long_shadow"), indexOf(entries, "fx:extrude"));
    EXPECT_LT(indexOf(entries, "fx:gradient_overlay"), indexOf(entries, "recipe:shine"));
    EXPECT_LT(indexOf(entries, "fx:distress"), indexOf(entries, "style:dry_brush"));
    EXPECT_LT(indexOf(entries, "style:dry_brush"), indexOf(entries, "fx:drips"));
}

// R42: English and Japanese names, aliases and preset names
TEST(TitleAddCatalogTest, SearchFindsEnglishAndJapanese) {
    const std::vector<AddEntry> entries = buildAddEntries("", textTarget());
    auto has = [&](const std::string& query, const std::string& id) {
        const std::vector<std::string> hits = matching(entries, query);
        return std::find(hits.begin(), hits.end(), id) != hits.end();
    };
    EXPECT_TRUE(has("power glow", "fx:light_glow"));
    EXPECT_TRUE(has("\xE3\x81\x8B\xE3\x81\x99\xE3\x82\x8C", "style:dry_brush")) << "かすれ finds Dry Brush";
    EXPECT_TRUE(has("\xE3\x81\x8B\xE3\x81\x99\xE3\x82\x8C", "fx:distress")) << "and Distress";
    EXPECT_TRUE(has("focus lines", "gen:speed_lines:focus"));
    EXPECT_TRUE(has("\xE9\x9B\x86\xE4\xB8\xAD\xE7\xB7\x9A", "gen:speed_lines:focus")) << "集中線 finds Focus Lines";
    EXPECT_TRUE(has("\xE9\x9B\x86\xE4\xB8\xAD\xE7\xB7\x9A", "follow:speed_lines:focus")) << "and its following preset";
    // "fire" lists the generator section's Fire
    bool generator = false;
    for (const std::string& id : matching(entries, "fire")) generator = generator || id.rfind("gen:fire", 0) == 0;
    EXPECT_TRUE(generator);
    EXPECT_TRUE(matching(entries, "zzzz qqqq").empty());
}

// D6: a generator entry adds a Generator layer, wherever the palette lists it; a following one
// follows the target and is placed beside it (no `after`)
TEST(TitleAddCatalogTest, GeneratorsAreLayerRows) {
    const std::vector<AddEntry> entries = buildAddEntries("", textTarget());
    size_t generators = 0;
    for (const AddEntry& e : entries) {
        if (e.kind != PreviewItem::Kind::Generator) continue;
        generators++;
        ASSERT_EQ(e.ops.size(), 1u) << e.id;
        EXPECT_EQ(e.ops[0].value("op", std::string()), "add_layer") << e.id;
        EXPECT_EQ(e.ops[0].value("type", std::string()), "generator") << e.id;
        EXPECT_TRUE(e.addsLayer) << e.id;
        EXPECT_FALSE(e.cost.empty()) << e.id << ": R41's class on the row";
    }
    EXPECT_GT(generators, 20u);
    const AddEntry* follow = find(entries, "follow:fire:behind");
    ASSERT_NE(follow, nullptr);
    EXPECT_EQ(follow->ops[0].value("follow", std::string()), "id:t1");
    EXPECT_FALSE(follow->ops[0].contains("after"));
    EXPECT_EQ(follow->ops[0].value("preset", std::string()), "behind");
    EXPECT_NE(follow->note.find("below"), std::string::npos) << "Flames Behind is placed below: " << follow->note;
    // a generator that follows nothing goes above the selection
    const AddEntry* fill = find(entries, "gen:fill");
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->ops[0].value("after", std::string()), "id:t1");
}

// §6.10 style settings: they switch the member on and name the section; never an effect row.
// Glint (3D highlights) only on a Dimension 3D layer, Dry Brush only on a 2D one.
TEST(TitleAddCatalogTest, StyleSettingsSwitchTheirMember) {
    const std::vector<AddEntry> flat = buildAddEntries("effects", textTarget(false));
    const AddEntry* dry = find(flat, "style:dry_brush");
    ASSERT_NE(dry, nullptr);
    ASSERT_EQ(dry->ops.size(), 1u);
    EXPECT_EQ(dry->ops[0].value("op", std::string()), "set");
    EXPECT_TRUE(dry->ops[0]["values"].value("style.erode.enabled", false));
    EXPECT_EQ(dry->openSections, (std::vector<std::string>{"Appearance", "Erode"}));
    EXPECT_EQ(find(flat, "style:glint_3d"), nullptr);
    const std::vector<AddEntry> deep = buildAddEntries("effects", textTarget(true));
    ASSERT_NE(find(deep, "style:glint_3d"), nullptr);
    EXPECT_EQ(find(deep, "style:dry_brush"), nullptr);
}

// R17: with parts selected the effects go to the parts; style settings and following generators
// are the whole layer's and are not offered
// a row's side text stops short of the cost badge (it ran under it)
TEST(TitleAddCatalogTest, SideTextIsCutToFitOnCharacterBoundaries) {
    // one unit per whole character: a cut inside a character's bytes draws no glyph for it
    const auto measure = [](const std::string& text) {
        float n = 0.0f;
        for (size_t i = 0; i < text.size();) {
            const unsigned char c = (unsigned char)text[i];
            const size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (i + length <= text.size()) n += 1.0f;
            i += length;
        }
        return n;
    };
    EXPECT_EQ(fitAddEntryText("short", 10.0f, measure), "short");
    EXPECT_EQ(fitAddEntryText("a longer description", 10.0f, measure), "a longe...");
    const std::string cut = fitAddEntryText("\xE3\x81\x8B\xE3\x81\x99\xE3\x82\x8C\xE3\x81\x9F\xE5\xAD\x97", 4.0f, measure);
    EXPECT_EQ(cut, "\xE3\x81\x8B...") << "a Japanese description is cut between characters";
    EXPECT_EQ(fitAddEntryText("anything", 2.0f, measure), "");
}

TEST(TitleAddCatalogTest, PartsTarget) {
    AddTarget target = textTarget();
    target.parts = {NodeRef{NodeKind::Part, "t1", "face", {}}};
    const std::vector<AddEntry> entries = buildAddEntries("effects", target);
    const AddEntry* glow = find(entries, "fx:outer_glow");
    ASSERT_NE(glow, nullptr);
    EXPECT_EQ(glow->ops[0].value("part", std::string()), "face");
    for (const AddEntry& e : entries) {
        EXPECT_NE(e.category, FOLLOW_CATEGORY);
        EXPECT_NE(e.id.rfind("style:", 0), 0u);
    }
    // nothing selected: no effects at all
    EXPECT_TRUE(buildAddEntries("effects", AddTarget()).empty());
}

// the entries' ops are S80/S83 ops: each commits as one batch (here through TitleArtOps), the
// recipe adds its effect, the following preset a Generator layer beside the text
TEST(TitleAddCatalogTest, EntriesApply) {
    registerTitleFxOps();
    ta::TitleArtDoc doc = ta::makeNewTitleArt();
    std::string title;
    ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
        if (title.empty()) title = layer.value("id", std::string());
        return true;
    });
    AddTarget target = textTarget();
    target.layers = {title};
    const std::vector<AddEntry> entries = buildAddEntries("", target);
    for (const char* id : {"recipe:long_shadow", "follow:fire:behind", "fx:light_glow", "style:dry_brush", "layer:shape:star"}) {
        const AddEntry* e = find(entries, id);
        ASSERT_NE(e, nullptr) << id;
        TitleOpsResult result = TitleArtOps::apply(doc, e->ops);
        EXPECT_TRUE(result.ok) << id << ": " << result.error();
        if (!result.ok) continue;
        size_t layers = 0;
        ta::forEachLayer(result.doc.root, [&](const ta::Json&, const ta::Json*, int) {
            layers++;
            return true;
        });
        EXPECT_EQ(layers, e->addsLayer ? 2u : 1u) << id;
    }
}
