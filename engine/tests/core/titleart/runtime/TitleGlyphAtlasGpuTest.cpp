// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Glyph Atlas on the headless device (docs/specs/S81-title-art-runtime.md R25-R31, AC11), with the
// module's own text (TitleTextLayout, the OFL test fonts):
// - a counter "1,234,567" composed from its glyph cells matches a Realtime render of the same text
//   within the approximate class's tolerance (below), and so does the next value, for which only the
//   new digit renders a cell and the change costs a compose (a handful of passes);
// - rims stay behind every face: with tight tracking a glyph's rim reaches under its neighbour's
//   face; the cells compose back cells first and match the merged render, and the red control
//   (TITLE_ART_TEST_GLYPH_STRING_ORDER: each glyph's rim and face together, in string order)
//   leaves rims over faces and falls outside the tolerance;
// - the atlas grows to a second page, frees the least recently used page no shown string uses,
//   and fails with R29's sentence when a string does not fit;
// - warp is skipped per glyph (the cells match a Realtime render with warp off); a plan that falls
//   back renders as Realtime.
// Tolerance of a glyph-safe look against the merged render (cells render at the string's
// sub-pixel phase to 1/32 px and are stored in RGBA8, linear premultiplied): mean difference <= 1/255, at
// most 1% of pixels over 8/255.

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

#include "ImageCompare.h"

#include <cctype>
#include <chrono>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr uint32_t UNLIMITED = 1000000000u;
    constexpr double TOLERANCE_MEAN = 1.0;
    constexpr double TOLERANCE_OVER = 0.01;
    constexpr const char* TEXT_LAYER = "00000001";

    Json rimOf(const char* id, double width, const char* color) {
        return Json{{"id", id}, {"width", width}, {"material", solidFace(color)}};
    }

    // a counter: a text layer the exposed `text` drives, over a plate, under a frame line
    std::shared_ptr<const TitleArtDoc> counterDoc(double size, double tracking = 0.0, Json extra = Json::object(), bool plates = true) {
        Json text{{"id", TEXT_LAYER}, {"type", "text"}, {"name", "Score"}, {"text", "1,234,567"}, {"font", fontRef(SOURCE_SANS)},
                  {"size", size}, {"tracking", tracking}, {"align", "start"},
                  {"style", {{"face", solidFace("#ffd040")}, {"rims", Json::array({rimOf("00000101", 6, "#802000")})}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) text[it.key()] = it.value();
        Json layers = Json::array();
        if (plates) layers.push_back(shapeLayer("00000002", "rect", 150, solidFace("#203060"), {{"aspect", 4.0}}));
        layers.push_back(text);
        if (plates) layers.push_back(shapeLayer("00000003", "rect", 24, solidFace("#ffffff"), {{"aspect", 20.0}, {"transform", {{"position", {0, -70}}}}}));
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(720, 220);
        root["layers"] = layers;
        root["exposed"] = Json::array({Json{{"name", "text"}, {"type", "text"}, {"targets", Json::array({Json{{"layer", TEXT_LAYER}, {"field", "text"}}})}}});
        TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return std::make_shared<const TitleArtDoc>(std::move(parsed.doc));
    }

    std::vector<FieldOverride> textOverride(const std::string& text) {
        return {FieldOverride{TEXT_LAYER, "text", Json(text)}};
    }

    // the pixels of a live's front image (straight-alpha sRGB, as S80's output)
    Pixels frontPixels(const TitleArtLive& live) {
        const TitleDisplayImage& front = live.front();
        Pixels out;
        out.width = front.width;
        out.height = front.height;
        out.premultiplied = false;
        auto* target = front.target.as<SokolTitleTarget>();
        if (!target) {
            ADD_FAILURE() << "the front image has no target";
            return out;
        }
        EXPECT_TRUE(jasperengine::test::readTexture(target->texture(), front.width, front.height, out.bytes));
        for (uint8_t b : out.bytes) out.rgba.push_back(b / 255.0f);
        return out;
    }

    int runFrames(TitleArtLive& live, const TitleJobFactory& factory, int maxFrames = 100000) {
        TitleLiveLimits limits;
        double clock = 0.0;
        const auto start = std::chrono::steady_clock::now();
        int frames = 0;
        while (live.hasWork() && frames < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, factory, UNLIMITED, 1.0);
            SystemRender::commit();
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            frames++;
        }
        return frames;
    }

    TitleArtLiveState state(int pageSize = 1024, int maxPages = 4, uint32_t flags = 0) {
        TitleArtLiveState s;
        s.label = "TitleArt/Score";
        s.atlasPageSize = pageSize;
        s.atlasMaxPages = maxPages;
        s.testFlags = flags;
        return s;
    }

    uint64_t stateHash(const std::string& text, uint32_t flags = 0) {
        return hashCombine(fnv1a64(text), flags + 1);
    }

    class TitleGlyphAtlasGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }

        // S80's merged render of the document with `overrides`, as a Realtime title shows it
        Pixels realtime(const std::shared_ptr<const TitleArtDoc>& doc, std::vector<FieldOverride> overrides) {
            TitleArtRenderer renderer;
            RenderRequest request;
            request.doc = doc;
            request.assets = &assets;
            request.overrides = std::move(overrides);
            return render(renderer, request);
        }

        // a Glyph Atlas live of `doc` showing `text`
        std::unique_ptr<TitleArtLive> glyphLive(const std::shared_ptr<const TitleArtDoc>& doc) {
            auto live = std::make_unique<TitleArtLive>("assets/score.titleart", TitleArtRenderMode::GLYPH_ATLAS, doc, 1);
            live->setGlyphPlan(planTitleGlyphAtlas(*doc));
            return live;
        }

        TitleJobFactory factory() { return makeRendererJobFactory(renderer, &assets); }

        // a failed comparison's images, for the record: build/test-results/glyph-atlas/
        static void dump(const Pixels& glyph, const Pixels& merged, const std::string& what) {
            const std::filesystem::path dir = std::filesystem::path(jasperengine::test::testPaths().sourceDir).parent_path().parent_path().parent_path() /
                                              "build" / "test-results" / "glyph-atlas";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::string name;
            for (char c : what) name += std::isalnum((unsigned char)c) ? c : '_';
            jasperengine::test::writePng((dir / (name + "-glyph.png")).string(), glyph.bytes, glyph.width, glyph.height);
            jasperengine::test::writePng((dir / (name + "-merged.png")).string(), merged.bytes, merged.width, merged.height);
        }

        void expectWithin(const Pixels& a, const Pixels& b, const char* what) {
            const Difference d = compare(a, b);
            if (d.mean > TOLERANCE_MEAN || d.overFraction > TOLERANCE_OVER) dump(a, b, what);
            EXPECT_LE(d.mean, TOLERANCE_MEAN) << what << ": mean " << d.mean << " max " << d.max << " over " << d.overFraction;
            EXPECT_LE(d.overFraction, TOLERANCE_OVER) << what << ": mean " << d.mean << " max " << d.max << " over " << d.overFraction;
            std::printf("[glyph atlas] %s: mean %.3f max %.1f over8 %.4f\n", what, d.mean, d.max, d.overFraction);
        }

        TestAssetResolver assets;
        TitleArtRenderer renderer;
    };

}

TEST_F(TitleGlyphAtlasGpu, CounterMatchesRealtimeAndAChangeComposesOnly) {
    // a counter's usual set-up: tabular figures, no kerning, so a new value keeps every glyph's place
    auto doc = counterDoc(110, 0, {{"features", {{"tnum", 1}}}, {"kerning", false}});
    auto live = glyphLive(doc);
    ASSERT_TRUE(live->glyphAtlasPlan().fallback.empty()) << live->glyphAtlasPlan().fallback;
    live->setState(state(), stateHash("1,234,567"), textOverride("1,234,567"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    ASSERT_TRUE(live->glyphAtlas()) << "rendered per glyph, not as Realtime";
    const TitleGlyphAtlas& atlas = *live->glyphAtlas();
    EXPECT_GE(atlas.stats().cellRenders, 8u) << "1 to 7 and the comma";
    EXPECT_LE(atlas.stats().cellRenders, 9u) << "one cell per cluster and sub-pixel phase: the second comma at most one more";
    expectWithin(frontPixels(*live), realtime(doc, textOverride("1,234,567")), "1,234,567");

    // the next value: one new glyph, then a compose
    const uint64_t cellsBefore = atlas.stats().cellRenders;
    live->setState(state(), stateHash("1,234,568"), textOverride("1,234,568"));
    const int frames = runFrames(*live, factory());
    EXPECT_EQ(atlas.stats().cellRenders, cellsBefore + 1) << "only the 8 renders";
    EXPECT_LE(frames, 2);
    expectWithin(frontPixels(*live), realtime(doc, textOverride("1,234,568")), "1,234,568");
    const uint64_t cellsAfter = atlas.stats().cellRenders;

    // every glyph cached (the first value again, not kept as an image): the compose alone
    const uint64_t composes = atlas.stats().composes;
    while (live->evictOldest()) {
    }
    live->setState(state(), stateHash("1,234,567"), textOverride("1,234,567"));
    runFrames(*live, factory());
    EXPECT_EQ(atlas.stats().composes, composes + 1) << "a new image, not a cache hit";
    EXPECT_EQ(atlas.stats().cellRenders, cellsAfter) << "no cell renders";
    EXPECT_LE(atlas.stats().composePasses, 4u + 2u * (uint64_t)atlas.pageCount())
        << "clear, back plate, a cell pass per page run (back cells, then front cells), front plate, output";
    expectWithin(frontPixels(*live), realtime(doc, textOverride("1,234,567")), "1,234,567 again");
}

TEST_F(TitleGlyphAtlasGpu, RimsStayBehindEveryFace) {
    // tracking pulls the glyphs together: each rim reaches under the next glyph's face
    auto doc = counterDoc(110, -200, {{"style", {{"face", solidFace("#ffd040")}, {"rims", Json::array({rimOf("00000101", 16, "#802000")})}}}},
                          false);
    const Pixels merged = realtime(doc, textOverride("88888"));
    auto live = glyphLive(doc);
    live->setState(state(), stateHash("88888"), textOverride("88888"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    expectWithin(frontPixels(*live), merged, "88888 back cells first");

    // red control: rim and face of each glyph together, in string order
    auto red = glyphLive(doc);
    red->setState(state(1024, 4, TITLE_ART_TEST_GLYPH_STRING_ORDER), stateHash("88888", TITLE_ART_TEST_GLYPH_STRING_ORDER), textOverride("88888"));
    runFrames(*red, factory());
    ASSERT_TRUE(red->front().valid()) << red->status().error;
    const Difference d = compare(frontPixels(*red), merged);
    std::printf("[glyph atlas] 88888 string order (red control): mean %.3f max %.1f over8 %.4f\n", d.mean, d.max, d.overFraction);
    EXPECT_TRUE(d.overFraction > TOLERANCE_OVER || d.mean > TOLERANCE_MEAN) << "string order covers faces with the next rim";
}

TEST_F(TitleGlyphAtlasGpu, AtlasGrowsEvictsAndSaysWhenFull) {
    auto doc = counterDoc(150, 0, {}, false);
    // growth: ten digits of 150 px do not fit one 1024 page (cells keep S80's 64-pixel ROI grid)
    auto live = glyphLive(doc);
    live->setState(state(1024, 8), stateHash("0123456789"), textOverride("0123456789"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    EXPECT_GE(live->glyphAtlas()->pageCount(), 2);
    EXPECT_EQ(live->status().atlasPages, (uint32_t)live->glyphAtlas()->pageCount());

    // eviction: two pages, strings of new letters; a page no shown string uses is freed
    auto small = glyphLive(doc);
    const char* strings[] = {"ABCD", "EFGH", "JKLM", "NPQR", "STUV", "WXYZ", "abcd", "efgh"};
    for (const char* s : strings) {
        small->setState(state(1024, 2), stateHash(s), textOverride(s));
        runFrames(*small, factory());
        ASSERT_FALSE(small->status().failed) << s << ": " << small->status().error;
        ASSERT_LE(small->glyphAtlas()->pageCount(), 2);
    }
    EXPECT_GT(small->glyphAtlas()->stats().evictions, 0u);

    // full: one page cannot hold a string's glyphs
    auto full = glyphLive(doc);
    full->setState(state(512, 1), stateHash("ABCDEFGHJKLMNPQRSTUV"), textOverride("ABCDEFGHJKLMNPQRSTUV"));
    runFrames(*full, factory(), 2000);
    EXPECT_TRUE(full->status().failed);
    EXPECT_NE(full->status().error.find("Title Art 'Score': the glyph atlas is full (1 pages of 512). Raise Max Pages, lower Resolution "
                                        "Scale, or reduce the Characters."),
              std::string::npos)
        << full->status().error;
}

TEST_F(TitleGlyphAtlasGpu, WarpIsSkippedAndFallbacksRenderAsRealtime) {
    auto warped = counterDoc(110, 0, {{"warp", {{"style", "arc"}}}});
    const TitleGlyphAtlasPlan plan = planTitleGlyphAtlas(*warped);
    ASSERT_EQ(plan.unsafeLabels(), std::vector<std::string>{"Warp"});
    auto live = glyphLive(warped);
    live->setState(state(), stateHash("1,234"), textOverride("1,234"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    EXPECT_EQ(live->status().glyphUnsafe, std::vector<std::string>{"Warp"});
    std::vector<FieldOverride> flat = textOverride("1,234");
    flat.push_back(FieldOverride{TEXT_LAYER, "warp.style", Json("none")});
    expectWithin(frontPixels(*live), realtime(warped, flat), "warp skipped");

    // R30: a plan that falls back (3D text) renders as Realtime: no atlas
    auto doc = counterDoc(110);
    auto fallback = std::make_unique<TitleArtLive>("assets/score.titleart", TitleArtRenderMode::GLYPH_ATLAS, doc, 1);
    TitleGlyphAtlasPlan three;
    three.textLayer = TEXT_LAYER;
    three.fallback = "3D text";
    fallback->setGlyphPlan(three);
    fallback->setState(state(), stateHash("12"), textOverride("12"));
    runFrames(*fallback, factory());
    ASSERT_TRUE(fallback->front().valid()) << fallback->status().error;
    EXPECT_FALSE(fallback->glyphAtlas());
    EXPECT_EQ(fallback->status().glyphFallback, "3D text");
}
