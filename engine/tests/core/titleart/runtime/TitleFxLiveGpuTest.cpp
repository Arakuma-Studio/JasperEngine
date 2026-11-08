// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's live variants run by S81's live compose (docs/specs/S83-title-effects-generators.md AC17,
// AC29; S81 R9) on the headless device, through a Realtime title (TitleArtLive and the
// scheduler, as a game runs them): every L param of every effect and generator with a live
// variant, exposed and changed to three values, composes in one frame with no re-render and no
// readback, runs fewer passes than a re-render, and shows what a full re-render at that value
// shows (S83 §6.5's F1, F0 for scale_split). S83 §6.12's minimum set plays live frame after frame
// as a keyed animation does; a live effect followed by one without a live variant re-renders with
// the suffix rule's reason (its red control).

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleGenTestSupport.h"
#include "../support/TitleStandardTitle.h"
#include "../support/TitleTestAssets.h"

#include "titleart/TitleArtHash.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleLiveCompose.h"

#include <chrono>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr const char* TITLE = "00000001";
    constexpr const char* GENERATOR = "00000301";
    constexpr float SCALE = 0.25f;
    constexpr uint32_t UNLIMITED = 1000000000u;

    // the document of one definition: the standard title with the effect, or the generator's
    // first preset over (following) it; its L param exposed as "p"
    struct Case {
        std::string label;
        std::shared_ptr<const TitleArtDoc> doc;
        std::string layer;
        std::string field;
        const ParamDef* param = nullptr;
        bool scaleSplit = false;
    };

    std::shared_ptr<const TitleArtDoc> exposing(TitleArtDoc doc, const std::string& layer, const std::string& field) {
        doc.root["exposed"] = Json::array({Json{{"name", "p"}, {"type", "number"},
                                                {"targets", Json::array({Json{{"layer", layer}, {"field", field}}})}}});
        TitleArtParseResult parsed = parseTitleArtJson(doc.root);
        EXPECT_TRUE(parsed.ok) << parsed.error.message;
        return std::make_shared<const TitleArtDoc>(parsed.doc);
    }

    Case effectCase(const EffectDef& def, const char* key, std::vector<Json> before = {}, std::vector<Json> after = {}) {
        Case c;
        c.label = std::string(def.type) + "." + key;
        std::vector<Json> effects = std::move(before);
        const int index = (int)effects.size();
        effects.push_back(standardEffect("00000201", def.type));
        for (Json& e : after) effects.push_back(std::move(e));
        c.layer = TITLE;
        c.field = "effects[" + std::to_string(index) + "].params." + key;
        c.doc = exposing(standardTitleDocument(effects), c.layer, c.field);
        c.param = findParam(def.params, key);
        c.scaleSplit = def.live.kind == LiveKind::ScaleSplit;
        return c;
    }

    Case generatorCase(const GeneratorDef& def, const char* key) {
        Case c;
        c.label = std::string(def.type) + "." + key;
        const Json presets = loadGeneratorPresets(def.type);
        c.layer = GENERATOR;
        c.field = std::string("gen.") + key;
        c.doc = exposing(generatorPresetDocument(def.type, presets.empty() ? Json::object() : presets[0]), c.layer, c.field);
        c.param = findParam(def.params, key);
        return c;
    }

    // three values of a param, inside its range and apart from each other
    std::vector<Json> valuesOf(const ParamDef& p, const Json& current) {
        const bool bounded = p.min < p.max;
        auto at = [&](double t) {
            const double v = bounded ? p.min + (p.max - p.min) * t : (current.is_number() ? current.get<double>() : 1.0) * (0.5 + t);
            return p.kind == ParamKind::Integer ? Json((int64_t)std::llround(v)) : Json(v);
        };
        switch (p.kind) {
            case ParamKind::Number:
            case ParamKind::Integer: return {at(0.2), at(0.5), at(0.8)};
            case ParamKind::Bool: return {Json(true), Json(false), Json(true)};
            case ParamKind::Color: return {Json("#ff8040"), Json("#40a0ff"), Json("#ffffff")};
            case ParamKind::Vec2: {
                const double x = current.is_array() && current.size() == 2 ? current[0].get<double>() : 0.0;
                const double y = current.is_array() && current.size() == 2 ? current[1].get<double>() : 0.0;
                return {Json::array({x + 24.0, y}), Json::array({x, y - 16.0}), Json::array({x - 12.0, y + 30.0})};
            }
            case ParamKind::Enum: {
                std::vector<Json> out;
                for (size_t i = 0; i < p.options.size() && out.size() < 3; i++) out.push_back(Json(p.options[i].value));
                return out;
            }
            default: return {};
        }
    }

    Pixels frontPixels(const TitleArtLive& live) {
        const TitleDisplayImage& front = live.front();
        Pixels out;
        out.width = front.width;
        out.height = front.height;
        auto* target = front.target.as<SokolTitleTarget>();
        if (!target) {
            ADD_FAILURE() << "the front image has no target";
            return out;
        }
        EXPECT_TRUE(jasperengine::test::readTexture(target->texture(), front.width, front.height, out.bytes));
        for (uint8_t b : out.bytes) out.rgba.push_back(b / 255.0f);
        return out;
    }

    TitleArtLiveState stateOf() {
        TitleArtLiveState s;
        s.label = "TitleArt/Live";
        s.resolution = SCALE;
        return s;
    }

    // frames of the scheduler until the live has no work (programs compile on the way)
    int frames(TitleArtLive& live, const TitleJobFactory& factory, int maxFrames = 100000) {
        TitleLiveLimits limits;
        static double clock = 0.0;
        TitleScheduleContext context;
        context.factory = factory;
        context.budgetUs = UNLIMITED;
        const auto start = std::chrono::steady_clock::now();
        int n = 0;
        while (live.hasWork() && n < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, context);
            SystemRender::commit();
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
            n++;
        }
        return n;
    }

    uint64_t hashOf(const std::vector<FieldOverride>& overrides) {
        uint64_t h = 7;
        for (const FieldOverride& o : overrides) h = hashCombine(h, fnv1a64(o.layerId + "|" + o.path + "|" + o.value.dump()));
        return h;
    }

    class TitleFxLiveGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
        TestAssetResolver assets;
        TitleArtRenderer renderer;          // the title's
        TitleArtRenderer reference;         // the re-renders it is compared with

        // the full re-render of `overrides`, and the passes it ran
        Pixels rerender(const Case& c, const std::vector<FieldOverride>& overrides, uint64_t& passes) {
            RenderRequest r;
            r.doc = c.doc;
            r.pxScale = SCALE;
            r.overrides = overrides;
            r.assets = &assets;
            std::unique_ptr<TitleRenderJob> job;
            Pixels p = render(reference, r, &job);
            passes = job ? job->passesTotal() : 0;
            return p;
        }

        // the title renders once, then each value composes live and matches a re-render
        void runCase(const Case& c) {
            SCOPED_TRACE(c.label);
            ASSERT_NE(c.param, nullptr);
            const TitleJobFactory factory = makeRendererJobFactory(renderer, &assets);
            TitleArtLive live("assets/live.titleart", TitleArtRenderMode::REALTIME, c.doc, hashTitleArt(*c.doc).document);
            ASSERT_FALSE(live.liveKeeps().empty()) << "the exposed L param is live";
            live.setState(stateOf(), 1, {});
            frames(live, factory);
            ASSERT_TRUE(live.front().valid()) << live.status().error;
            ASSERT_FALSE(live.front().liveInputs.empty()) << "the render kept the live inputs";
            const Json* layer = findLayer(c.doc->root, c.layer);
            ASSERT_NE(layer, nullptr);
            const Json* stored = getTitleArtField(*layer, "layer", c.field);
            const Json current = stored ? *stored : c.param->defaultValue;
            // a live frame skips the content (effects) or the followed layer (following generators)
            const bool skips = c.layer == TITLE || !layer->value("follow", std::string()).empty();
            for (const Json& value : valuesOf(*c.param, current)) {
                SCOPED_TRACE(value.dump());
                const std::vector<FieldOverride> overrides{FieldOverride{c.layer, c.field, value}};
                const uint64_t hash = hashOf(overrides);
                const uint64_t renders = live.rendersDone(), composes = live.liveComposesDone();
                const uint64_t readbacks = gridReadbacks();
                const uint64_t passesBefore = renderer.stats().passes;
                live.setState(stateOf(), hash, overrides);
                EXPECT_TRUE(live.liveWaiting()) << "a change of an L param composes live: " << live.status().liveFallback;
                const int took = frames(live, factory);
                const uint64_t livePasses = renderer.stats().passes - passesBefore;
                ASSERT_EQ(live.front().hash, hash) << live.status().error;
                EXPECT_EQ(live.rendersDone(), renders) << "no re-render: " << live.status().liveFallback;
                EXPECT_EQ(live.liveComposesDone(), composes + 1);
                EXPECT_LE(took, 3) << "composed in its frame (once its programs are built)";
                EXPECT_EQ(gridReadbacks(), readbacks) << "a live frame reads nothing back";
                uint64_t fullPasses = 0;
                const Pixels expected = rerender(c, overrides, fullPasses);
                if (skips) EXPECT_LT(livePasses, fullPasses) << "the live frame runs the live effect and what is above it, not the title";
                else EXPECT_LE(livePasses, fullPasses);
                const Difference d = compare(frontPixels(live), expected);
                // S83 §6.5: F0 for scale_split, F1 for kernels
                EXPECT_LE(d.mean, c.scaleSplit ? 1.0 : 1.5);
                EXPECT_LE(d.max, c.scaleSplit ? 48.0 : 64.0);
                EXPECT_LE(d.overFraction, c.scaleSplit ? 0.005 : 0.02);
            }
        }
    };

}

TEST_F(TitleFxLiveGpu, EveryLiveParamComposesLikeARerender) {
    int cases = 0;
    for (const EffectDef* def : TitleEffectRegistry::get().effects()) {
        if (def->live.kind == LiveKind::None) continue;
        for (const char* key : def->live.params) {
            runCase(effectCase(*def, key));
            cases++;
        }
    }
    for (const GeneratorDef* def : TitleEffectRegistry::get().generators()) {
        if (def->live.kind == LiveKind::None) continue;
        for (const char* key : def->live.params) {
            runCase(generatorCase(*def, key));
            cases++;
        }
    }
    std::printf("[live] %d live params composed at 3 values each\n", cases);
    EXPECT_GT(cases, 40);
}

TEST_F(TitleFxLiveGpu, S58MinimumSet) {
    // S83 §6.12 (S85's §6.2 table): each keyed over frames plays live every frame
    struct Entry {
        const char* type;
        const char* param;
    };
    const Entry set[] = {{"light_glow", "intensity"}, {"chromatic_aberration", "amount"}, {"wave_warp", "phase"},      {"wave_warp", "amplitude"},
                         {"shatter", "progress"},     {"glitch", "seed"},                 {"glitch", "amount"},        {"sunburst", "rotation"},
                         {"shockwave", "radius"},     {"shockwave", "width"},             {"shockwave", "flash"},      {"gradient_overlay", "position"},
                         {"gradient_overlay", "amount"}, {"gradient_overlay", "angle"},   {"gradient_overlay", "offset"}, {"gradient_overlay", "scale"},
                         {"outer_glow", "intensity"}, {"outer_glow", "opacity"},          {"drop_shadow", "opacity"},  {"outline", "opacity"},
                         {"color_overlay", "opacity"}, {"color_overlay", "color"},        {"pattern_fill", "offset"},  {"pattern_fill", "rotation"}};
    for (const Entry& e : set) {
        Case c;
        if (const EffectDef* def = findEffect(e.type)) {
            c = effectCase(*def, e.param);
        } else if (const GeneratorDef* gen = findGenerator(e.type)) {
            c = generatorCase(*gen, e.param);
        } else {
            ADD_FAILURE() << e.type << " is not registered";
            continue;
        }
        SCOPED_TRACE(c.label);
        ASSERT_NE(c.param, nullptr);
        const TitleJobFactory factory = makeRendererJobFactory(renderer, &assets);
        TitleArtLive live("assets/live.titleart", TitleArtRenderMode::REALTIME, c.doc, hashTitleArt(*c.doc).document);
        live.setState(stateOf(), 1, {});
        frames(live, factory);
        ASSERT_TRUE(live.front().valid()) << live.status().error;
        // a keyed track: a new value every frame (five frames of a ramp through the three values)
        const std::vector<Json> values = valuesOf(*c.param, Json());
        const uint64_t renders = live.rendersDone();
        for (int frame = 0; frame < 5; frame++) {
            const std::vector<FieldOverride> overrides{FieldOverride{c.layer, c.field, values[(size_t)frame % values.size()]}};
            live.setState(stateOf(), hashOf(overrides) + (uint64_t)frame * 0x100000000ull, overrides);
            EXPECT_TRUE(live.liveWaiting()) << "frame " << frame << ": " << live.status().liveFallback;
            frames(live, factory);
            EXPECT_TRUE(live.status().liveFallback.empty()) << live.status().liveFallback;
            if (frame == 4) {
                uint64_t passes = 0;
                const Difference d = compare(frontPixels(live), rerender(c, overrides, passes));
                EXPECT_LE(d.mean, 1.5);
                EXPECT_LE(d.max, 64.0);
                EXPECT_LE(d.overFraction, 0.02);
            }
        }
        EXPECT_EQ(live.rendersDone(), renders) << "every frame composed live";
        EXPECT_EQ(live.liveComposesDone(), 5u);
    }
}

TEST_F(TitleFxLiveGpu, ALiveEffectBeforeOneWithoutALiveVariantReRenders) {
    // S83 §6.12's suffix rule: a sweep followed by an enabled Gaussian Blur re-renders, saying why
    const EffectDef* sweep = findEffect("gradient_overlay");
    ASSERT_NE(sweep, nullptr);
    ASSERT_NE(findEffect("gaussian_blur"), nullptr);
    const Case c = effectCase(*sweep, "position", {}, {standardEffect("00000202", "gaussian_blur")});
    const TitleJobFactory factory = makeRendererJobFactory(renderer, &assets);
    TitleArtLive live("assets/live.titleart", TitleArtRenderMode::REALTIME, c.doc, hashTitleArt(*c.doc).document);
    live.setState(stateOf(), 1, {});
    frames(live, factory);
    ASSERT_TRUE(live.front().valid()) << live.status().error;
    const uint64_t renders = live.rendersDone();
    const std::vector<FieldOverride> overrides{FieldOverride{c.layer, c.field, Json(0.7)}};
    live.setState(stateOf(), hashOf(overrides), overrides);
    EXPECT_FALSE(live.liveWaiting());
    EXPECT_NE(live.status().liveFallback.find("Gaussian Blur after Gradient Overlay"), std::string::npos) << live.status().liveFallback;
    EXPECT_NE(live.status().liveFallback.find("has no live variant: the change re-renders"), std::string::npos);
    frames(live, factory);
    EXPECT_EQ(live.rendersDone(), renders + 1) << "it re-rendered";
    EXPECT_EQ(live.liveComposesDone(), 0u);
}

TEST_F(TitleFxLiveGpu, OverTheLiveBudgetReRenders) {
    // S81 R9: a live compose predicted over the title's budget re-renders (fx_live_budget)
    const Case c = effectCase(*findEffect("light_glow"), "intensity");
    const TitleJobFactory factory = makeRendererJobFactory(renderer, &assets);
    TitleArtLive live("assets/live.titleart", TitleArtRenderMode::REALTIME, c.doc, hashTitleArt(*c.doc).document);
    live.setState(stateOf(), 1, {});
    frames(live, factory);
    ASSERT_TRUE(live.front().valid()) << live.status().error;
    live.setLiveBudgetMs(1e-6);
    const std::vector<FieldOverride> overrides{FieldOverride{c.layer, c.field, Json(2.0)}};
    live.setState(stateOf(), hashOf(overrides), overrides);
    EXPECT_FALSE(live.liveWaiting());
    EXPECT_NE(live.status().liveFallback.find("over the title's live budget"), std::string::npos) << live.status().liveFallback;
}
