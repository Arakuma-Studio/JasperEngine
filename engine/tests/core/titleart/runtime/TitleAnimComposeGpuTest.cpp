// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 R14 (AC8), R15 on the headless device: a Realtime title's frame while layer-live and glyph
// tracks play. The composed frame matches the renders it stands in for: a split layer moved,
// faded, tinted and flashed against the same values as a full render, and two text layers with
// glyph poses at rest against the merged render (within Glyph Atlas's approximate-class tolerance:
// cells are drawn at whole pixels). The red control is the empty pose (no split: the frame shows
// the layer at rest, not at the sampled values).

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include "subsystem/TitleArtRuntime.h"
#include "titleanim/TitleAnimPlayer.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleAnimCompose.h"
#include "titleart/runtime/TitleGlyphAtlas.h"

#include "ImageCompare.h"

#include <chrono>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr uint32_t UNLIMITED = 1000000000u;
    constexpr double TOLERANCE_MEAN = 1.5;
    constexpr double TOLERANCE_OVER = 0.03;
    constexpr const char* MOVING = "00000002";

    Json rimOf(const char* id, double width, const char* color) {
        return Json{{"id", id}, {"width", width}, {"material", solidFace(color)}};
    }

    std::shared_ptr<const TitleArtDoc> splitDoc() {
        Json layers = Json::array();
        layers.push_back(shapeLayer("00000001", "rect", 300, solidFace("#203060"), {{"aspect", 3.0}}));
        layers.push_back(shapeLayer(MOVING, "ellipse", 120, solidFace("#ff8020")));
        layers.push_back(shapeLayer("00000003", "rect", 24, solidFace("#ffffff"), {{"aspect", 20.0}, {"transform", {{"position", {0, -90}}}}}));
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(720, 360);
        root["layers"] = layers;
        TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return std::make_shared<const TitleArtDoc>(std::move(parsed.doc));
    }

    std::shared_ptr<const TitleArtDoc> twoTextDoc() {
        auto text = [](const char* id, const char* rim, const char* value, double y, const char* color){
            return Json{{"id", id}, {"type", "text"}, {"name", id}, {"text", value}, {"font", fontRef(SOURCE_SANS)}, {"size", 90}, {"align", "start"},
                        {"style", {{"face", solidFace(color)}, {"rims", Json::array({rimOf(rim, 4, "#000000")})}}},
                        {"transform", {{"position", {-200, y}}}}};
        };
        Json layers = Json::array();
        layers.push_back(text("00000001", "00000101", "AB", 60, "#ffd040"));
        layers.push_back(text("00000002", "00000201", "CD", -60, "#40d0ff"));
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(720, 360);
        root["layers"] = layers;
        TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return std::make_shared<const TitleArtDoc>(std::move(parsed.doc));
    }

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

    int runFrames(TitleArtLive& live, const TitleJobFactory& factory) {
        TitleLiveLimits limits;
        double clock = 0.0;
        int frames = 0;
        const auto start = std::chrono::steady_clock::now();
        while (live.hasWork() && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
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

    class TitleAnimComposeGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }

        TitleJobFactory factory() { return makeRendererJobFactory(renderer, &assets); }

        Pixels composed(const std::shared_ptr<const TitleArtDoc>& doc, std::shared_ptr<const TitleComposePose> pose, uint64_t hash) {
            auto live = std::make_unique<TitleArtLive>("assets/compose.titleart", TitleArtRenderMode::REALTIME, doc, 1);
            live->setComposePose(std::move(pose));
            TitleArtLiveState s;
            s.label = "TitleArt/Compose";
            s.atlasPageSize = 1024;
            s.atlasMaxPages = 4;
            live->setState(s, hash, {});
            runFrames(*live, factory());
            EXPECT_TRUE(live->front().valid()) << live->status().error;
            return frontPixels(*live);
        }

        Pixels full(const std::shared_ptr<const TitleArtDoc>& doc, std::vector<FieldOverride> overrides) {
            TitleArtRenderer full;
            RenderRequest request;
            request.doc = doc;
            request.assets = &assets;
            request.overrides = std::move(overrides);
            return render(full, request);
        }

        void expectWithin(const Pixels& a, const Pixels& b, const char* what) {
            const Difference d = compare(a, b);
            std::printf("[title compose] %s: mean %.3f max %.1f over8 %.4f\n", what, d.mean, d.max, d.overFraction);
            EXPECT_LE(d.mean, TOLERANCE_MEAN) << what;
            EXPECT_LE(d.overFraction, TOLERANCE_OVER) << what;
        }

        TestAssetResolver assets;
        TitleArtRenderer renderer;
    };

}

TEST_F(TitleAnimComposeGpu, LayerSplitEqualsRender) {
    auto doc = splitDoc();
    auto pose = std::make_shared<TitleComposePose>();
    pose->layers[MOVING] = {{"transform.position", Json::array({60.0, 40.0})}, {"opacity", 0.6}, {"tint", "#ff80ff"}};
    const Pixels shown = composed(doc, pose, 1);
    const Pixels reference = full(doc, {FieldOverride{MOVING, "transform.position", Json::array({60.0, 40.0})},
                                        FieldOverride{MOVING, "opacity", 0.6},
                                        FieldOverride{MOVING, "tint", "#ff80ff"}});
    expectWithin(shown, reference, "a split layer moved, faded and tinted");

    // red: no split, the rest pose stands in for the sampled one
    auto none = std::make_shared<TitleComposePose>();
    none->layers[MOVING] = {};
    const Difference d = compare(composed(doc, none, 2), reference);
    std::printf("[title compose] rest pose against the moved render (red control): mean %.3f over8 %.4f\n", d.mean, d.overFraction);
    EXPECT_TRUE(d.mean > TOLERANCE_MEAN || d.overFraction > TOLERANCE_OVER);

    // rotation and scale, about the layer's own origin
    auto spun = std::make_shared<TitleComposePose>();
    spun->layers[MOVING] = {{"transform.rotation", 25.0}, {"transform.scale", 1.4}};
    expectWithin(composed(doc, spun, 3), full(doc, {FieldOverride{MOVING, "transform.rotation", 25.0}, FieldOverride{MOVING, "transform.scale", 1.4}}),
                 "a split layer rotated and scaled");
}

TEST_F(TitleAnimComposeGpu, TwoTextLayersAtRestMatchTheMergedRender) {
    auto doc = twoTextDoc();
    auto pose = std::make_shared<TitleComposePose>();
    GlyphPose identity;
    pose->glyphs["00000001"] = {{0u, identity}, {1u, identity}};
    pose->glyphs["00000002"] = {{0u, identity}, {1u, identity}};
    const Pixels shown = composed(doc, pose, 11);
    expectWithin(shown, full(doc, {}), "two text layers' cells at rest");

    // each layer moves its own first glyph; the other text layer is untouched
    GlyphPose moved = identity;
    moved.offset[0] = 60.0;
    auto animated = std::make_shared<TitleComposePose>();
    animated->glyphs["00000001"] = {{0u, moved}, {1u, identity}};
    animated->glyphs["00000002"] = {{0u, identity}, {1u, identity}};
    const Pixels a = composed(doc, animated, 12);
    ASSERT_EQ(a.width, shown.width);
    auto rowsDiffer = [&](int y0, int y1){
        double sum = 0.0;
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < a.width; x++) sum += std::fabs((double)a.bytes[(size_t)(y * a.width + x) * 4 + 3] - shown.bytes[(size_t)(y * a.width + x) * 4 + 3]);
        return sum;
    };
    EXPECT_GT(rowsDiffer(0, a.height / 2), 1000.0) << "the first layer's glyph moved";
    EXPECT_LT(rowsDiffer(a.height / 2, a.height), 200.0) << "the second layer stayed";
}

// R18: `prerender` renders its sample times ahead, and playback draws them from the frame cache
TEST_F(TitleAnimComposeGpu, PrerenderFillsTheCacheAndPlaysWithoutRendering) {
    Json layers = Json::array();
    layers.push_back(shapeLayer("00000001", "ellipse", 100, solidFace("#ff8020")));
    Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
    root["canvas"] = canvas(256, 128);
    root["layers"] = layers;
    Json animation = {{"name", "P"}, {"duration", 0.2}, {"rerender", "prerender"}, {"rerender_fps", 10},
                      {"tracks", Json::array({Json{{"layer", "00000001"}, {"field", "size"}, {"keys", Json::array({Json::array({0, 100}), Json::array({0.2, 140})})}}})}};
    root["animation"] = {{"animations", Json::array({animation})}};
    TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    auto doc = std::make_shared<const TitleArtDoc>(std::move(parsed.doc));

    auto library = compileAnimations(*doc, nullptr, nullptr);
    titleanim::TitleAnimPlayer player;
    player.setLibrary(library);
    TitleArtLive live("assets/prerender.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    TitleArtLiveState state;
    state.label = "TitleArt/Prerender";
    auto frame = [&](){
        TitleArtRuntime::setAnimation(live, player, 1);
        TitleArtRuntime::setState(live, state);
        runFrames(live, factory());
    };
    frame();
    ASSERT_TRUE(TitleArtRuntime::prepareAnimation(live, player, "P"));
    EXPECT_TRUE(TitleArtRuntime::isPrerendering(live));
    for (int i = 0; i < 16 && TitleArtRuntime::isPrerendering(live); i++) frame();
    EXPECT_FALSE(TitleArtRuntime::isPrerendering(live));
    frame();

    // each sample of the playback is already rendered: no new step runs
    const uint64_t before = live.status().steps;
    ASSERT_TRUE(player.play("P", titleanim::TitleAnimPlayOptions()));
    for (int k = 0; k <= 2; k++){
        player.seek(k / 10.0);
        TitleArtRuntime::setAnimation(live, player, 1);
        TitleArtRuntime::setState(live, state);
        runFrames(live, factory());
    }
    EXPECT_EQ(live.status().steps, before) << "the frames came from the cache";

    // 60 s at 60 samples a second cannot fit the Cache Budget: it says so and plays live
    Json bigRoot = doc->root;
    bigRoot["animation"]["animations"][0]["duration"] = 60.0;
    bigRoot["animation"]["animations"][0]["rerender_fps"] = 60;
    TitleArtParseResult big = parseTitleArtJson(std::move(bigRoot));
    ASSERT_TRUE(big.ok) << big.error.toString();
    auto bigDoc = std::make_shared<const TitleArtDoc>(std::move(big.doc));
    titleanim::TitleAnimPlayer bigPlayer;
    bigPlayer.setLibrary(compileAnimations(*bigDoc, nullptr, nullptr));
    TitleArtLive bigLive("assets/prerender-big.titleart", TitleArtRenderMode::REALTIME, bigDoc, 2);
    EXPECT_FALSE(TitleArtRuntime::prepareAnimation(bigLive, bigPlayer, "P"));
    EXPECT_FALSE(TitleArtRuntime::isPrerendering(bigLive));
}
