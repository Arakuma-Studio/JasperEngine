// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A live title switched to another language on the headless device
// (docs/specs/S87-title-localisation-interchange.md AC8, R17): Realtime and Glyph Atlas titles in
// German and Japanese show what a one-shot render of that language shows, within S80's tolerance,
// and the previous language stays on screen until the new image is complete.

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

#include <chrono>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr uint32_t UNLIMITED = 1000000000u;
    constexpr double TOLERANCE_MEAN = 1.0;
    constexpr double TOLERANCE_OVER = 0.01;
    constexpr double SIZE = 96.0;
    constexpr const char* TEXT_LAYER = "00000001";

    std::shared_ptr<const TitleArtDoc> multilingual() {
        Json text{{"id", TEXT_LAYER}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"font", fontRef(SOURCE_SANS)},
                  {"size", SIZE}, {"align", "start"}, {"kerning", false},
                  {"style", {{"face", solidFace("#ffd040")},
                             {"rims", Json::array({Json{{"id", "00000101"}, {"width", 6.0}, {"material", solidFace("#802000")}}})}}},
                  {"translations", Json{{"de", Json{{"text", "GEWINN"}}},
                                        {"ja", Json{{"text", "大当たり"}, {"font", fontRef(DELA_GOTHIC)}}}}}};
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"},
                  {"source_language", "en"}, {"languages", Json::array({Json{{"code", "de"}}, Json{{"code", "ja"}}})}};
        root["canvas"] = canvas(640, 200);
        root["layers"] = Json::array({text});
        // what makes a title Glyph Atlas capable (S81 R25)
        root["exposed"] = Json::array({Json{{"name", "text"}, {"type", "text"}, {"targets", Json::array({Json{{"layer", TEXT_LAYER}, {"field", "text"}}})}}});
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

    int runFrames(TitleArtLive& live, const TitleJobFactory& factory, uint32_t budgetUs = UNLIMITED, int maxFrames = 100000) {
        TitleLiveLimits limits;
        double clock = 0.0;
        const auto start = std::chrono::steady_clock::now();
        int frames = 0;
        while (live.hasWork() && frames < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, factory, budgetUs, 1.0);
            SystemRender::commit();
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            frames++;
        }
        return frames;
    }

    TitleArtLiveState state() {
        TitleArtLiveState s;
        s.label = "TitleArt/Language";
        return s;
    }

    uint64_t hashOf(const std::string& code) { return fnv1a64(code) + 1; }

    class TitleRealtimeLanguageGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }

        // the one-shot render of a language, with `overrides` (Glyph Atlas's pixel snap)
        Pixels oneShot(const std::shared_ptr<const TitleArtDoc>& doc, const std::string& code, std::vector<FieldOverride> overrides = {}) {
            TitleArtRenderer fresh;
            RenderRequest request;
            request.doc = doc;
            request.assets = &assets;
            request.language = code;
            request.overrides = std::move(overrides);
            return render(fresh, request);
        }

        void expectWithin(const Pixels& a, const Pixels& b, const std::string& what) {
            const Difference d = compare(a, b);
            EXPECT_LE(d.mean, TOLERANCE_MEAN) << what << ": mean " << d.mean << " max " << d.max << " over " << d.overFraction;
            EXPECT_LE(d.overFraction, TOLERANCE_OVER) << what << ": mean " << d.mean << " max " << d.max << " over " << d.overFraction;
            std::printf("[title language] %s: mean %.3f max %.1f over8 %.4f\n", what.c_str(), d.mean, d.max, d.overFraction);
        }

        // the images really differ: a language that did not switch fails here
        void expectDifferent(const Pixels& a, const Pixels& b, const std::string& what) {
            EXPECT_GT(compare(a, b).overFraction, 0.02) << what;
        }

        TestAssetResolver assets;
        TitleArtRenderer renderer;
        TitleJobFactory factory() { return makeRendererJobFactory(renderer, &assets); }
    };

}

TEST_F(TitleRealtimeLanguageGpu, RealtimeShowsEachLanguage) {
    auto doc = multilingual();
    TitleArtLive live("assets/language.titleart", TitleArtRenderMode::REALTIME, doc, 1);
    live.setState(state(), hashOf(""), {});
    runFrames(live, factory());
    ASSERT_TRUE(live.front().valid()) << live.status().error;
    const Pixels english = frontPixels(live);
    expectWithin(english, oneShot(doc, ""), "Realtime en");
    Pixels previous = english;
    for (const char* code : {"de", "ja"}) {
        ASSERT_TRUE(live.setLanguage(code));
        live.setState(state(), hashOf(code), {});
        // the render is sliced: while it runs, the last language stays (R17)
        runFrames(live, factory(), 200, 1);
        if (live.hasWork()) {
            EXPECT_TRUE(identical(frontPixels(live), previous)) << code << ": the previous language shows until the new image is complete";
        }
        runFrames(live, factory());
        ASSERT_TRUE(live.front().valid()) << live.status().error;
        const Pixels shown = frontPixels(live);
        expectWithin(shown, oneShot(doc, code), std::string("Realtime ") + code);
        expectDifferent(shown, previous, std::string(code) + " against the language before");
        previous = shown;
    }
}

TEST_F(TitleRealtimeLanguageGpu, GlyphAtlasShowsEachLanguage) {
    auto doc = multilingual();
    TitleArtLive live("assets/language.titleart", TitleArtRenderMode::GLYPH_ATLAS, doc, 1);
    live.setGlyphPlan(planTitleGlyphAtlas(*doc));
    live.setState(state(), hashOf(""), {});
    runFrames(live, factory());
    ASSERT_TRUE(live.front().valid()) << live.status().error;
    ASSERT_TRUE(live.glyphAtlas()) << "rendered per glyph: " << live.glyphAtlasPlan().fallback;
    Pixels previous = frontPixels(live);
    const struct {
        const char* code;
        int characters;
    } languages[] = {{"de", 6}, {"ja", 4}};
    for (const auto& language : languages) {
        ASSERT_TRUE(live.setLanguage(language.code));
        ASSERT_TRUE(live.glyphAtlasPlan().fallback.empty()) << live.glyphAtlasPlan().fallback;
        live.setState(state(), hashOf(language.code), {});
        runFrames(live, factory());
        ASSERT_TRUE(live.front().valid()) << live.status().error;
        ASSERT_TRUE(live.glyphAtlas());
        const double snapX = live.glyphAtlas()->stats().snapX, snapY = live.glyphAtlas()->stats().snapY;
        // the reference sits where Glyph Atlas snapped the string (TitleGlyphAtlasGpuTest's rule)
        std::vector<FieldOverride> snapped;
        for (int i = 0; i < language.characters; i++) {
            snapped.push_back(FieldOverride{TEXT_LAYER, "chars." + std::to_string(i) + ".offset", Json::array({snapX / SIZE * 100.0, -snapY / SIZE * 100.0})});
        }
        const Pixels shown = frontPixels(live);
        expectWithin(shown, oneShot(doc, language.code, snapped), std::string("Glyph Atlas ") + language.code);
        expectDifferent(shown, previous, std::string(language.code) + " against the language before");
        previous = shown;
    }
}
