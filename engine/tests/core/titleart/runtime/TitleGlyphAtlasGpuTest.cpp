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
//   back renders as Realtime;
// - per-character scale, rotation and face materials, and a text layer's opacity and multiply
//   blend, match the merged render;
// - cells keep only their reach (ten 150 px digits fit one 1,024 page), and a cell's face only its
//   ink: a layer's wide glow goes to the back part alone, so a size 220 title with Light Glow's
//   default 200 px radius fits 1,024 pages; a cell wider than a page says so and how to fix it;
// - a prebaked atlas (R32: snapshot, files, restore) serves a new title's first value with no
//   cell render, where the same title without it renders its cells.
// Tolerance of a glyph-safe look against the merged render (cells render at the string's
// sub-pixel phase to 1/32 px and are stored in RGBA8, linear premultiplied): mean difference <= 1/255, at
// most 1% of pixels over 8/255. Glyph Atlas snaps the string to the pixel grid (under half a pixel
// from Realtime's sub-pixel place, so a counter's glyphs keep their cells): the reference is the
// Realtime render of the same text moved by that snap (every character's offset, which moves no
// layer, so nothing is resampled); the difference against Realtime's own place is printed.

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"
#include "util/TitleArtBakeInfo.h"

#include "ImageCompare.h"

#include <limits>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>    // getpid
#else
#include <unistd.h>
#endif

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
    std::shared_ptr<const TitleArtDoc> counterDoc(double size, double tracking = 0.0, Json extra = Json::object(), bool plates = true,
                                                  int canvasWidth = 720, int canvasHeight = 220) {
        Json text{{"id", TEXT_LAYER}, {"type", "text"}, {"name", "Score"}, {"text", "1,234,567"}, {"font", fontRef(SOURCE_SANS)},
                  {"size", size}, {"tracking", tracking}, {"align", "start"},
                  {"style", {{"face", solidFace("#ffd040")}, {"rims", Json::array({rimOf("00000101", 6, "#802000")})}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) text[it.key()] = it.value();
        Json layers = Json::array();
        if (plates) layers.push_back(shapeLayer("00000002", "rect", 150, solidFace("#203060"), {{"aspect", 4.0}}));
        layers.push_back(text);
        if (plates) layers.push_back(shapeLayer("00000003", "rect", 24, solidFace("#ffffff"), {{"aspect", 20.0}, {"transform", {{"position", {0, -70}}}}}));
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(canvasWidth, canvasHeight);
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

    // frames are not the limit: a hardware device builds its programs on a worker for far longer
    // than a hundred thousand empty frames last (the job only waits); the wall clock below is
    int runFrames(TitleArtLive& live, const TitleJobFactory& factory, int maxFrames = std::numeric_limits<int>::max()) {
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

        // the Realtime render of `text` placed where Glyph Atlas snapped it (`size` the layer's)
        Pixels realtimeAt(const std::shared_ptr<const TitleArtDoc>& doc, const std::string& text, const TitleGlyphAtlas& atlas, double size,
                          std::vector<FieldOverride> extra = {}) {
            const double snapX = atlas.stats().snapX, snapY = atlas.stats().snapY;
            EXPECT_LE(std::abs(snapX), 0.5);
            EXPECT_LE(std::abs(snapY), 0.5);
            std::vector<FieldOverride> overrides = textOverride(text);
            for (size_t i = 0; i < text.size(); i++) {
                overrides.push_back(FieldOverride{TEXT_LAYER, "chars." + std::to_string(i) + ".offset",
                                                  Json::array({snapX / size * 100.0, -snapY / size * 100.0})});
            }
            for (FieldOverride& o : extra) overrides.push_back(std::move(o));
            return realtime(doc, overrides);
        }

        // the difference against Realtime's own (unsnapped) place, for the record
        void printUnsnapped(const Pixels& glyph, const std::shared_ptr<const TitleArtDoc>& doc, std::vector<FieldOverride> overrides, const char* what) {
            const Difference d = compare(glyph, realtime(doc, std::move(overrides)));
            std::printf("[glyph atlas] %s against Realtime's own place: mean %.3f max %.1f over8 %.4f\n", what, d.mean, d.max, d.overFraction);
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
    expectWithin(frontPixels(*live), realtimeAt(doc, "1,234,567", atlas, 110), "1,234,567");
    printUnsnapped(frontPixels(*live), doc, textOverride("1,234,567"), "1,234,567");

    // the next value: one new glyph, then a compose
    const uint64_t cellsBefore = atlas.stats().cellRenders;
    live->setState(state(), stateHash("1,234,568"), textOverride("1,234,568"));
    const int frames = runFrames(*live, factory());
    EXPECT_EQ(atlas.stats().cellRenders, cellsBefore + 1) << "only the 8 renders";
    EXPECT_LE(frames, 2);
    expectWithin(frontPixels(*live), realtimeAt(doc, "1,234,568", atlas, 110), "1,234,568");
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
    expectWithin(frontPixels(*live), realtimeAt(doc, "1,234,567", atlas, 110), "1,234,567 again");
}

// S86's one-new-draw-a-frame rule skips a pass and expects it to run again; the job's compose and
// page copies never do. From no draw state known yet, every glyph must still reach the image.
TEST_F(TitleGlyphAtlasGpu, FirstDrawsOfACleanDeviceLeaveNoHoles) {
    auto doc = counterDoc(110, 0, {{"features", {{"tnum", 1}}}, {"kerning", false}});
    TitleFirstDraws::resetForTesting(false);
    auto live = glyphLive(doc);
    live->setState(state(), stateHash("1,234,567"), textOverride("1,234,567"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    ASSERT_TRUE(live->glyphAtlas());
    const Pixels image = frontPixels(*live);
    expectWithin(image, realtimeAt(doc, "1,234,567", *live->glyphAtlas(), 110), "from a clean device");
}

TEST_F(TitleGlyphAtlasGpu, RimsStayBehindEveryFace) {
    // tracking pulls the glyphs together: each rim reaches under the next glyph's face
    auto doc = counterDoc(110, -200, {{"style", {{"face", solidFace("#ffd040")}, {"rims", Json::array({rimOf("00000101", 16, "#802000")})}}}},
                          false);
    auto live = glyphLive(doc);
    live->setState(state(), stateHash("88888"), textOverride("88888"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    const Pixels merged = realtimeAt(doc, "88888", *live->glyphAtlas(), 110);
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
    // growth: ten digits of 150 px do not fit one 512 page
    auto live = glyphLive(doc);
    live->setState(state(512, 8), stateHash("0123456789"), textOverride("0123456789"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    EXPECT_GE(live->glyphAtlas()->pageCount(), 2);
    EXPECT_EQ(live->status().atlasPages, (uint32_t)live->glyphAtlas()->pageCount());

    // eviction: two pages, a new letter each time; the page the shown letter is not on is freed
    auto small = glyphLive(doc);
    for (char letter = 'A'; letter <= 'Z'; letter++) {
        const std::string s(1, letter);
        small->setState(state(512, 2), stateHash(s), textOverride(s));
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

TEST_F(TitleGlyphAtlasGpu, ACellsFaceKeepsItsInkNotTheLayersGlow) {
    // the Title Designer's reference look: size 220, a rim, Light Glow at its default 200 px radius, a drop shadow
    const Json effects = Json::array({Json{{"id", "0000d001"}, {"type", "light_glow"}, {"params", Json::object()}},
                                      Json{{"id", "0000d002"}, {"type", "drop_shadow"}, {"params", Json::object()}}});
    auto doc = counterDoc(220, 0, {{"effects", effects}}, false, 1920, 1080);

    auto wide = glyphLive(doc);
    wide->setState(state(2048, 4), stateHash("JACKPOT"), textOverride("JACKPOT"));
    runFrames(*wide, factory());
    ASSERT_FALSE(wide->status().failed) << wide->status().error;
    TitleGlyphAtlasSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(wide->glyphAtlas()->snapshot(snapshot, error)) << error;
    int cells = 0;
    for (const Json& cell : snapshot.manifest["cells"]) {
        if (cell["blank"].get<bool>()) continue;
        cells++;
        const int backWidth = cell["back"][2], backHeight = cell["back"][3], frontWidth = cell["front"][2], frontHeight = cell["front"][3];
        std::printf("[glyph atlas] cell %s: back %d x %d, front %d x %d, page %d\n", cell["key"].get<std::string>().c_str(), backWidth, backHeight,
                    frontWidth, frontHeight, cell["page"].get<int>());
        // the glow's 200 px on each side belong to the back part; the face is its glyph and the rim
        EXPECT_LE(frontWidth, backWidth - 300) << cell.dump();
        EXPECT_LE(frontHeight, backHeight - 300) << cell.dump();
        EXPECT_LE(backWidth + 1 + frontWidth, 1024) << "a cell must fit the default page: " << cell.dump();
    }
    EXPECT_EQ(cells, 7);

    // one cell a 1,024 page, seven pages: the default four say R29's sentence, Max Pages 8 is enough
    auto four = glyphLive(doc);
    four->setState(state(1024, 4), stateHash("JACKPOT"), textOverride("JACKPOT"));
    runFrames(*four, factory());
    EXPECT_TRUE(four->status().failed);
    EXPECT_NE(four->status().error.find("the glyph atlas is full (4 pages of 1024). Raise Max Pages"), std::string::npos) << four->status().error;
    auto eight = glyphLive(doc);
    eight->setState(state(1024, 8), stateHash("JACKPOT"), textOverride("JACKPOT"));
    runFrames(*eight, factory());
    ASSERT_FALSE(eight->status().failed) << eight->status().error;
    ASSERT_TRUE(eight->front().valid());

    // a cell wider than a page: more pages cannot help, and the error says what can
    auto small = glyphLive(doc);
    small->setState(state(512, 8), stateHash("JACKPOT"), textOverride("JACKPOT"));
    runFrames(*small, factory());
    EXPECT_TRUE(small->status().failed);
    EXPECT_NE(small->status().error.find("does not fit an atlas page of 512"), std::string::npos) << small->status().error;
    EXPECT_NE(small->status().error.find("Raise Atlas Page Size"), std::string::npos) << small->status().error;
    EXPECT_EQ(small->status().error.find("Raise Max Pages"), std::string::npos) << small->status().error;
    EXPECT_EQ(small->glyphAtlas()->pageCount(), 0) << "no page is made for a cell no page can hold";

    // the face cropped to its ink loses nothing: one glyph (no neighbours to overlap) against Realtime
    const std::string letter = "A";
    auto single = glyphLive(doc);
    single->setState(state(2048, 4), stateHash(letter), textOverride(letter));
    runFrames(*single, factory());
    ASSERT_TRUE(single->front().valid()) << single->status().error;
    const Pixels glyph = frontPixels(*single);
    const Pixels merged = realtimeAt(doc, letter, *single->glyphAtlas(), 220);
    expectWithin(glyph, merged, "a wide glow, one glyph");
    // the tolerance above is diluted by this canvas: a face clipped to 87 x 113 px still meets it (measured: max 209, 93 uncut)
    EXPECT_LT(compare(glyph, merged).max, 150.0) << "the face lost pixels to its crop";
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
    expectWithin(frontPixels(*live), realtimeAt(warped, "1,234", *live->glyphAtlas(), 110, {FieldOverride{TEXT_LAYER, "warp.style", Json("none")}}),
                 "warp skipped");

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

TEST_F(TitleGlyphAtlasGpu, CellsKeepOnlyTheirReach) {
    // S80's 64-pixel ROI grid would need two 1,024 pages for these (the red control)
    auto doc = counterDoc(150, 0, {}, false);
    auto live = glyphLive(doc);
    live->setState(state(1024, 8), stateHash("0123456789"), textOverride("0123456789"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    EXPECT_EQ(live->glyphAtlas()->pageCount(), 1);
    expectWithin(frontPixels(*live), realtimeAt(doc, "0123456789", *live->glyphAtlas(), 150), "0123456789 tight cells");

    // a look that reaches far (a drop shadow) keeps its reach
    Json shadow = Json::array({Json{{"id", "00000201"}, {"type", "drop_shadow"}, {"params", {{"distance", 14}, {"size", 10}}}}});
    auto shadowed = counterDoc(110, 300, {{"effects", shadow}}, false);
    auto withShadow = glyphLive(shadowed);
    withShadow->setState(state(), stateHash("123"), textOverride("123"));
    runFrames(*withShadow, factory());
    ASSERT_TRUE(withShadow->front().valid()) << withShadow->status().error;
    expectWithin(frontPixels(*withShadow), realtimeAt(shadowed, "123", *withShadow->glyphAtlas(), 110), "123 drop shadow");
}

TEST_F(TitleGlyphAtlasGpu, PerCharacterLooks) {
    // the third character larger, turned and green; the line a little larger
    Json chars{{"2", {{"scale", 1.4}, {"rotation", 20}, {"face", solidFace("#40ff80")}}}};
    auto adjusted = counterDoc(100, 0, {{"chars", chars}, {"lines", {{"0", {{"scale", 1.1}}}}}}, false);
    auto live = glyphLive(adjusted);
    live->setState(state(), stateHash("12345"), textOverride("12345"));
    runFrames(*live, factory());
    ASSERT_TRUE(live->front().valid()) << live->status().error;
    const Pixels glyph = frontPixels(*live);
    expectWithin(glyph, realtimeAt(adjusted, "12345", *live->glyphAtlas(), 100), "12345 per-character looks");
    // the adjustments show: without them the render differs
    auto plain = counterDoc(100, 0, {}, false);
    const Difference without = compare(glyph, realtime(plain, textOverride("12345")));
    EXPECT_GT(without.overFraction, TOLERANCE_OVER * 4) << "the looks are visible";
}

TEST_F(TitleGlyphAtlasGpu, TheTextLayersOpacityAndBlend) {
    // the text layer at 70 % opacity, multiplied over its plate
    auto blended = counterDoc(110, 0, {{"blend", "multiply"}, {"opacity", 0.7}});
    auto blend = glyphLive(blended);
    blend->setState(state(), stateHash("1,234"), textOverride("1,234"));
    runFrames(*blend, factory());
    ASSERT_TRUE(blend->front().valid()) << blend->status().error;
    expectWithin(frontPixels(*blend), realtimeAt(blended, "1,234", *blend->glyphAtlas(), 110), "1,234 multiply 70 %");
}

// S85 R15: the per-glyph values of an animation compose as moved, tinted, flashed cells
TEST_F(TitleGlyphAtlasGpu, GlyphPosesMoveTintAndFlashCells) {
    auto doc = counterDoc(110, 0, {{"style", {{"face", solidFace("#ffffff")}, {"rims", Json::array()}}}}, false);
    auto runWith = [&](std::map<uint32_t, GlyphPose> poses, const std::string& key) {
        auto live = glyphLive(doc);
        live->setGlyphPoses(std::make_shared<const std::map<uint32_t, GlyphPose>>(std::move(poses)));
        live->setState(state(), stateHash(key), textOverride("88"));
        runFrames(*live, factory());
        EXPECT_TRUE(live->front().valid()) << live->status().error;
        return frontPixels(*live);
    };
    const Pixels rest = runWith({}, "rest");
    ASSERT_GT(rest.width, 0);
    auto centroidX = [](const Pixels& p) {
        double sum = 0.0, weight = 0.0;
        for (int y = 0; y < p.height; y++) {
            for (int x = 0; x < p.width; x++) {
                const double a = p.bytes[(size_t)(y * p.width + x) * 4 + 3] / 255.0;
                sum += x * a;
                weight += a;
            }
        }
        return weight > 0.0 ? sum / weight : 0.0;
    };

    // an identity pose is the rest's compose
    GlyphPose identity;
    identity.cluster = 0;
    expectWithin(runWith({{0u, identity}}, "identity"), rest, "identity pose against no pose");

    // the first glyph moves right by 50% of the size (55 px); the centroid moves (red: the
    // program's move dropped would leave it at the rest's place)
    GlyphPose moved = identity;
    moved.offset[0] = 50.0;
    const Pixels shifted = runWith({{0u, moved}}, "moved");
    EXPECT_GT(centroidX(shifted), centroidX(rest) + 5.0) << "the first glyph's offset moves the title's mass right";

    // opacity 0: only the second glyph is left, so about half the ink
    GlyphPose hidden = identity;
    hidden.opacity = 0.0;
    const Pixels gone = runWith({{0u, hidden}}, "hidden");
    auto inkOf = [](const Pixels& p) {
        double sum = 0.0;
        for (size_t i = 3; i < p.bytes.size(); i += 4) sum += p.bytes[i] / 255.0;
        return sum;
    };
    EXPECT_NEAR(inkOf(gone), inkOf(rest) * 0.5, inkOf(rest) * 0.05);

    // a red tint keeps red and removes green; a flash adds on the white face
    GlyphPose tinted = identity;
    tinted.tint[1] = 0.0;
    tinted.tint[2] = 0.0;
    const Pixels red = runWith({{0u, tinted}}, "tint");
    bool sawRed = false;
    for (size_t i = 0; i + 3 < red.bytes.size(); i += 4) {
        if (red.bytes[i + 3] > 200 && red.bytes[i] > 200 && red.bytes[i + 1] < 40) sawRed = true;
    }
    EXPECT_TRUE(sawRed) << "a pure red tint leaves red ink with no green";
}

TEST(TitleGlyphAtlasPrebake, StringsCoverEveryDigitPlace) {
    const std::vector<std::string> strings = titleGlyphPrebakeStrings("x10");
    EXPECT_EQ(strings.size(), 1u + 9u + 9u) << "the text, then nine other digits at each of its two digit places";
    EXPECT_EQ(strings.front(), "x10");
    EXPECT_NE(std::find(strings.begin(), strings.end(), "x90"), strings.end());
    EXPECT_NE(std::find(strings.begin(), strings.end(), "x17"), strings.end());
    EXPECT_EQ(std::find(strings.begin(), strings.end(), "y10"), strings.end()) << "letters stay";
}

TEST_F(TitleGlyphAtlasGpu, APrebakedAtlasServesTheFirstValue) {
    // the export prebakes in one call on a device that has drawn nothing: no frame will come
    TitleFirstDraws::resetForTesting(false);
    auto doc = counterDoc(110, 0, {{"features", {{"tnum", 1}}}, {"kerning", false}});
    // the export's side (R32): the initial value and every digit at its digit places, read back and
    // written as files
    auto exported = glyphLive(doc);
    std::string error;
    ASSERT_TRUE(prebakeTitleGlyphAtlas(*exported, factory(), state(), {}, TEXT_LAYER, titleGlyphPrebakeStrings("1,234,567"), 1024, error))
        << error;
    TitleGlyphAtlasSnapshot snapshot;
    ASSERT_TRUE(exported->glyphAtlas()->snapshot(snapshot, error)) << error;
    ASSERT_FALSE(snapshot.pages.empty());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("jasper-glyph-prebake-" + std::to_string(::getpid()));
    const std::string title = (dir / "score.titleart").string();
    const std::string manifest = titleArtGlyphPrebakePath(title, exported->glyphAtlas()->stats().styleHash);
    size_t bytes = 0;
    ASSERT_TRUE(TitleGlyphAtlas::writePrebaked(manifest, snapshot, bytes, error)) << error;
    EXPECT_GT(bytes, 0u);

    // the player's side: a new title beside the files shows a value it never rendered (two digits
    // changed) without rendering a glyph
    TitleArtLive player(title, TitleArtRenderMode::GLYPH_ATLAS, doc, 1);
    player.setGlyphPlan(planTitleGlyphAtlas(*doc));
    player.setState(state(), stateHash("9,234,507"), textOverride("9,234,507"));
    runFrames(player, factory());
    ASSERT_TRUE(player.front().valid()) << player.status().error;
    const TitleGlyphAtlas& atlas = *player.glyphAtlas();
    EXPECT_EQ(atlas.stats().restoredCells, exported->glyphAtlas()->cellCount());
    EXPECT_EQ(atlas.stats().cellRenders, 0u) << "every glyph came from the prebaked atlas";
    expectWithin(frontPixels(player), realtimeAt(doc, "9,234,507", atlas, 110), "9,234,507 from the prebaked atlas");

    // without the files the same title renders its cells
    TitleArtLive bare((dir / "other.titleart").string(), TitleArtRenderMode::GLYPH_ATLAS, doc, 1);
    bare.setGlyphPlan(planTitleGlyphAtlas(*doc));
    bare.setState(state(), stateHash("9,234,507"), textOverride("9,234,507"));
    runFrames(bare, factory());
    EXPECT_GT(bare.glyphAtlas()->stats().cellRenders, 0u);
    std::filesystem::remove_all(dir);
}
