// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Properties search's storage and ranking (docs/specs/S82-title-designer.md R26, AC9), on
// hand-built entries with the embedded search terms. The collect pass that fills the index from
// the drawn views is tested by TitlePropertyIndexTest.IndexFromViews (the window's tests).

#include "titleart/designer/TitlePropertyIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

using namespace jasperengine::editor::titledesigner;

namespace {

    IndexEntry row(const std::string& id, const std::string& label, std::vector<std::string> location,
                   std::vector<std::string> aliases = {}, std::vector<std::string> enumLabels = {}) {
        IndexEntry entry;
        entry.rowId = id;
        entry.label = label;
        entry.location = std::move(location);
        entry.aliases = std::move(aliases);
        entry.enumLabels = std::move(enumLabels);
        entry.kind = entry.enumLabels.empty() ? "number" : "enum";
        entry.fieldPath = id;
        return entry;
    }

    std::vector<std::string> ids(const PropertyIndex& index, const std::vector<SearchHit>& hits) {
        std::vector<std::string> out;
        for (const SearchHit& hit : hits) out.push_back(index.entry(hit.entry).rowId);
        return out;
    }

    std::set<std::string> idSet(const PropertyIndex& index, const std::vector<SearchHit>& hits) {
        const std::vector<std::string> list = ids(index, hits);
        return std::set<std::string>(list.begin(), list.end());
    }

    bool holds(const std::vector<std::string>& list, const std::string& id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    }

    // a layer's rows as the collect pass would record them
    PropertyIndex sampleIndex() {
        PropertyIndex index;
        index.add(row("rim1.width", "Width", {"Rim 1"}));
        index.add(row("rim2.width", "Width", {"Rim 2"}));
        index.add(row("rim1.color", "Color", {"Rim 1"}));
        index.add(row("rim2.bevel.size", "Bevel Size", {"Rim 2", "Bevel"}));
        index.add(row("glow.size", "Size", {"Outer Glow"}, {"サイズ"}));
        index.add(row("glow.softness", "Softness", {"Outer Glow"}));
        index.add(row("shadow.blur", "Blur", {"Drop Shadow"}));
        index.add(row("face.gloss", "Gloss", {"Face", "Finish"}, {}, {"None", "Soft", "Ring", "Sharp"}));
        index.add(row("text.h_scale", "Horizontal Scale", {"Text"}, {"水平比率"}));
        index.add(row("text.tracking", "Tracking", {"Text"}));
        return index;
    }

}

// Red control: without the synonym groups (an empty SearchTerms) "フチ" finds nothing.
TEST(TitlePropertyIndexTest, RimSynonymFindsRimRows) {
    const PropertyIndex index = sampleIndex();
    const std::vector<std::string> hits = ids(index, index.query("フチ", 0));
    EXPECT_TRUE(holds(hits, "rim1.width"));
    EXPECT_TRUE(holds(hits, "rim2.width"));
    EXPECT_FALSE(holds(hits, "glow.size"));
    EXPECT_FALSE(holds(hits, "text.tracking"));
    // half-width and hiragana spellings fold to the same query
    EXPECT_EQ(ids(index, index.query("ﾌﾁ", 0)), hits);
    EXPECT_EQ(ids(index, index.query("ふち", 0)), hits);

    const SearchTerms noTerms{};
    PropertyIndex withoutTerms(noTerms);
    withoutTerms.add(row("rim1.width", "Width", {"Rim 1"}));
    EXPECT_TRUE(withoutTerms.query("フチ", 0).empty());
}

// Red control: dropping "ぼかし" (or "softness") from the blur group makes the two sets differ.
TEST(TitlePropertyIndexTest, BlurInEnglishAndJapaneseFindsTheSameRows) {
    const PropertyIndex index = sampleIndex();
    const std::set<std::string> english = idSet(index, index.query("blur", 0));
    const std::set<std::string> japanese = idSet(index, index.query("ぼかし", 0));
    EXPECT_EQ(english, (std::set<std::string>{"shadow.blur", "glow.softness"}));
    EXPECT_EQ(japanese, english);
    EXPECT_EQ(idSet(index, index.query("ボカシ", 0)), english);
    // the row named Blur is the better hit for "blur"
    EXPECT_EQ(index.entry(index.query("blur", 0).front().entry).rowId, "shadow.blur");
}

// Red control: an offer index that ignores names_ja and the synonym groups finds no offer.
TEST(TitlePropertyIndexTest, GlowInJapaneseFindsTheOuterGlowOffer) {
    PropertyIndex index;
    index.addOffer({"effect", "drop_shadow", "Drop Shadow", {}, "Shadow & Depth"});
    index.addOffer({"effect", "outer_glow", "Outer Glow", {}, "Glow"});
    index.addOffer({"generator", "sunburst", "Sunburst", {}, "Lines"});
    // a type without a names_ja entry: found through the synonym group (光彩 = glow)
    index.addOffer({"effect", "test_halo_glow", "Halo Glow", {}, "Glow"});

    const std::vector<OfferHit> hits = index.queryOffers("光彩", 0);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(index.offer(hits[0].offer).type, "outer_glow");       // its Japanese name 光彩 (外側)
    EXPECT_EQ(hits[0].tier, searchtier::LABEL_PREFIX);
    EXPECT_EQ(index.offer(hits[1].offer).type, "test_halo_glow");
    EXPECT_EQ(hits[1].tier, searchtier::SYNONYM);
    // English still finds it, and the offers are separate from the rows
    ASSERT_FALSE(index.queryOffers("outer glow", 0).empty());
    EXPECT_EQ(index.offer(index.queryOffers("outer glow", 0).front().offer).type, "outer_glow");
    EXPECT_TRUE(index.query("光彩", 0).empty());
    EXPECT_EQ(index.queryOffers("ｻﾝﾊﾞｰｽﾄ", 0).size(), 1u);              // names_ja: サンバースト
}

// Red control: without the enum tier "Ring" finds nothing.
TEST(TitlePropertyIndexTest, EnumValueLabelFindsItsRow) {
    const PropertyIndex index = sampleIndex();
    const std::vector<SearchHit> hits = index.query("Ring", 0);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(index.entry(hits[0].entry).rowId, "face.gloss");
    EXPECT_EQ(hits[0].tier, searchtier::ENUM_VALUE);
    EXPECT_EQ(hits[0].matchedEnumLabel, "Ring");
    // with a word of its location
    const std::vector<SearchHit> finish = index.query("finish ring", 0);
    ASSERT_EQ(finish.size(), 1u);
    EXPECT_EQ(finish[0].matchedEnumLabel, "Ring");
}

// Red control: without the spread tier "rim width" finds only labels holding both words.
TEST(TitlePropertyIndexTest, WordsSpreadOverLabelAndLocation) {
    const PropertyIndex index = sampleIndex();
    const std::vector<SearchHit> hits = index.query("rim width", 0);
    EXPECT_EQ(ids(index, hits), (std::vector<std::string>{"rim1.width", "rim2.width"}));
    for (const SearchHit& hit : hits) EXPECT_EQ(hit.tier, searchtier::SPREAD);
    // the location alone does not make a hit of a label without any query word
    EXPECT_FALSE(holds(ids(index, hits), "rim1.color"));
    // Japanese aliases are searched with the English labels
    ASSERT_FALSE(index.query("水平比率", 0).empty());
    EXPECT_EQ(index.entry(index.query("水平比率", 0).front().entry).rowId, "text.h_scale");
    // and through the synonyms (長体 = horizontal scale)
    EXPECT_TRUE(holds(ids(index, index.query("長体", 0)), "text.h_scale"));
}

// Red control: sorting by fuzzyScore alone (no tiers) puts "Width of Rim" or "Trim Widths"
// above "Outer Rim Width"; an unstable sort may swap the two "Width" rows of equal score.
TEST(TitlePropertyIndexTest, TierOrderHolds) {
    PropertyIndex index;
    index.add(row("spread.1", "Width", {"Rim 1"}));
    index.add(row("spread.2", "Width", {"Rim 2"}));
    index.add(row("words", "Width of Rim", {}));
    index.add(row("inside", "Trim Widths", {}));
    index.add(row("word", "Outer Rim Width", {}));
    index.add(row("prefix", "Rim Width Mode", {}));
    index.add(row("equal", "Rim Width", {}));
    index.add(row("unrelated", "Opacity", {"Fill"}));

    const std::vector<SearchHit> hits = index.query("Rim Width", 0);
    EXPECT_EQ(ids(index, hits), (std::vector<std::string>{"equal", "prefix", "word", "inside", "words", "spread.1", "spread.2"}));
    std::vector<int> tiers;
    for (const SearchHit& hit : hits) tiers.push_back(hit.tier);
    EXPECT_EQ(tiers, (std::vector<int>{searchtier::LABEL_EQUAL, searchtier::LABEL_PREFIX, searchtier::LABEL_WORD,
                                       searchtier::LABEL_INSIDE, searchtier::ALL_WORDS, searchtier::SPREAD,
                                       searchtier::SPREAD}));
    // the limit keeps the best
    EXPECT_EQ(ids(index, index.query("rim width", 2)), (std::vector<std::string>{"equal", "prefix"}));
    EXPECT_TRUE(index.query("", 0).empty());
    EXPECT_TRUE(index.query("  ・ ", 0).empty());
    index.clear();
    EXPECT_EQ(index.size(), 0u);
    EXPECT_TRUE(index.query("rim", 0).empty());
}

// R26 / AC31: a query answers in well under 1 ms. Red control: folding the entries' texts per
// query instead of once in add() takes several milliseconds here.
TEST(TitlePropertyIndexTest, ThousandEntriesQueryUnderOneMillisecond) {
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr bool checkBudgets = false;
#else
    constexpr bool checkBudgets = true;
#endif
    const char* labels[] = {"Width", "Color", "Opacity", "Size", "Softness", "Angle", "Distance", "Blur", "Gloss",
                            "Horizontal Scale", "Tracking", "Spread"};
    const char* sections[] = {"Bevel", "Finish", "Outer Glow", "Drop Shadow", "Fill"};
    PropertyIndex index;
    for (int i = 0; i < 1000; i++) {
        const std::string label = labels[i % 12];
        std::vector<std::string> location{"Rim " + std::to_string(i / 12 % 16 + 1), sections[i % 5]};
        std::vector<std::string> enums;
        if (label == "Gloss") enums = {"None", "Soft", "Ring", "Sharp"};
        index.add(row("row." + std::to_string(i), label, location, {i % 3 == 0 ? "幅" : "サイズ"}, enums));
    }
    const char* queries[] = {"rim width", "ぼかし", "glow", "ring", "フチ", "horizontal scale"};
    size_t found = 0;
    const int rounds = 50;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; r++) {
        for (const char* q : queries) found += index.query(q, 30).size();
    }
    const double average = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                           (rounds * (sizeof(queries) / sizeof(queries[0])));
    std::printf("[ bench ] Properties search over 1000 entries: %.3f ms per query (budget 1)\n", average);
    EXPECT_GT(found, 0u);
    if (checkBudgets) {
        EXPECT_LT(average, 1.0);
    }
}
