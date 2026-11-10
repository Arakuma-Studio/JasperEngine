// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Art renderer on the headless device (docs/specs/S80-title-art-core.md R10-R19,
// R18a, R42-R44, R49; AC14, AC15, AC18-AC20, AC32, AC42): a job's members, time slicing equal to
// a one-shot render, determinism across fresh ids, the cache cascade's pass counts with every
// cached result equal to a fresh one, regions of interest and peak target memory, device loss,
// render variants (overrides, kept splits, glyph cells), an idle renderer holding nothing, what
// may move a cached node and a following generator's layer.
// The layers' text and geometry come from the tests' analytic provider (TitleRenderTestSupport.h).

#include "TitleRenderTestEffects.h"
#include "TitleRenderTestSupport.h"

#include "pool/ShaderPool.h"
#include "titleart/render/TitleElementVariant.h"
#include "titleart/render/TitlePrograms.h"

#include <chrono>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    Json goldMaterial() {
        return Json{{"fill", {{"kind", "linear"}, {"angle", 90},
                              {"gradient", {{"stops", Json::array({{{"pos", 0}, {"color", "#fff4b0"}}, {{"pos", 0.5}, {"color", "#e0a020"}}, {{"pos", 1}, {"color", "#7a4a08"}}})}}}}},
                    {"bevel", {{"enabled", true}, {"size", 4}, {"depth", 4}}},
                    {"finish", {{"enabled", true}}},
                    {"inner_glow", {{"enabled", true}, {"size", 3}}},
                    {"gloss", {{"enabled", true}}}};
    }

    Json rim(const char* id, double width, const char* color, bool bevel) {
        Json material = solidFace(color);
        if (bevel) material["bevel"] = {{"enabled", true}, {"size", 2}};
        return Json{{"id", id}, {"width", width}, {"material", material}};
    }

    // the classic-style fixture: a gold face with bevel, finish, inner glow and gloss; four
    // rims, three bevelled; Outer Glow and Drop Shadow at their defaults (§7)
    Json classicLayer(const char* id = "00000001", Json position = Json::array({0, 0})) {
        Json layer{{"id", id}, {"type", "text"}, {"name", "Title"}, {"text", "FFFF"}, {"size", 200},
                   {"transform", {{"position", position}}},
                   {"style", {{"face", goldMaterial()},
                              {"rims", Json::array({rim("00000011", 4, "#7a2a00", true), rim("00000012", 3, "#ffe080", true),
                                                    rim("00000013", 5, "#3a0a00", true), rim("00000014", 3, "#000000", false)})}}},
                   {"effects", Json::array({effect("00000021", "outer_glow", Json::object()), effect("00000022", "drop_shadow", Json::object())})}};
        return layer;
    }

    std::shared_ptr<const TitleArtDoc> classicDoc(int w = 1920, int h = 1080) {
        ensureTestEffects();
        return makeDoc({{"canvas", canvas(w, h)}, {"layers", Json::array({classicLayer()})}});
    }

    RenderRequest requestOf(std::shared_ptr<const TitleArtDoc> doc, float scale = 1.0f) {
        RenderRequest r;
        r.doc = std::move(doc);
        r.pxScale = scale;
        return r;
    }

    // the passes one render ran (the renderer's counter)
    uint64_t passesOf(TitleArtRenderer& renderer, const RenderRequest& request, Pixels* out = nullptr) {
        const uint64_t before = renderer.stats().passes;
        Pixels p = render(renderer, request);
        if (out) *out = std::move(p);
        return renderer.stats().passes - before;
    }

    std::shared_ptr<const TitleArtDoc> edited(const std::shared_ptr<const TitleArtDoc>& doc, const std::function<void(Json&)>& edit) {
        Json root = doc->root;
        edit(root);
        return makeDoc(root);
    }

    class TitleArtRendererGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            ensureTestEffects();
        }
    };

}

TEST_F(TitleArtRendererGpu, SolidShapeDrawsItsPixels) {
    auto renderer = makeRenderer();
    auto doc = makeDoc({{"canvas", canvas(256, 128)}, {"layers", Json::array({shapeLayer("00000001", "rect", 64, solidFace("#ff0000"))})}});
    std::unique_ptr<TitleRenderJob> job;
    Pixels p = render(*renderer, requestOf(doc), &job);
    ASSERT_EQ(p.width, 256);
    ASSERT_EQ(p.height, 128);
    EXPECT_EQ(job->image().frame, (TitleRectI{0, 0, 256, 128}));
    // the square's centre is opaque red; a corner is fully transparent (exact codes never move)
    const uint8_t* centre = &p.bytes[((size_t)64 * 256 + 128) * 4];
    EXPECT_EQ(centre[0], 255);
    EXPECT_EQ(centre[1], 0);
    EXPECT_EQ(centre[2], 0);
    EXPECT_EQ(centre[3], 255);
    EXPECT_EQ(p.bytes[3], 0);
    // its edges: the square spans x 96..160, y 32..96
    EXPECT_EQ(p.bytes[((size_t)64 * 256 + 95) * 4 + 3], 0);
    EXPECT_EQ(p.bytes[((size_t)64 * 256 + 97) * 4 + 3], 255);
    EXPECT_EQ(p.bytes[((size_t)31 * 256 + 128) * 4 + 3], 0);
    EXPECT_EQ(p.bytes[((size_t)33 * 256 + 128) * 4 + 3], 255);
    EXPECT_STREQ(renderer->stats().path, SystemRender::getFeatures().renderRGBA16F && SystemRender::getFeatures().filterRGBA16F ? "rgba16f" : "rgba8");
}

TEST_F(TitleArtRendererGpu, JobMembers) {
    auto renderer = makeRenderer();
    auto doc = classicDoc(640, 360);
    std::unique_ptr<TitleRenderJob> job = renderer->begin(requestOf(doc));
    ASSERT_TRUE(job);
    ASSERT_GT(job->passesTotal(), 10u);
    // every program the job binds, and only the renderer's
    const std::vector<ShaderKey> keys = job->shaderKeys();
    std::set<uint64_t> known;
    for (uint32_t i = TITLE_RENDER_PROGRAM_FIRST; i < TITLE_RENDER_PROGRAM_FIRST + TITLE_RENDER_PROGRAM_COUNT; i++) {
        known.insert(ShaderPool::getPropertiesFromKey(titleRenderProgramKey(i)));
    }
    std::set<uint32_t> used;
    for (const ShaderKey& k : keys) {
        EXPECT_EQ(ShaderPool::getShaderTypeFromKey(k), ShaderType::TITLE_ART);
        // ELEMENT's variants carry the material's bits (TitleElementVariant.h), no other program does
        const uint32_t properties = (uint32_t)ShaderPool::getPropertiesFromKey(k);
        const uint32_t program = properties & ~TITLE_ELEMENT_VARIANT_BITS;
        EXPECT_TRUE(known.count(program));
        if (program != (uint32_t)TitleRenderProgram::Element) { EXPECT_EQ(properties, program) << titleRenderProgramName(program); }
        used.insert(program);
    }
    for (TitleRenderProgram p : {TitleRenderProgram::Element, TitleRenderProgram::Place, TitleRenderProgram::Seed, TitleRenderProgram::Flood,
                                 TitleRenderProgram::Resolve, TitleRenderProgram::Height, TitleRenderProgram::Output, TitleRenderProgram::Copy}) {
        EXPECT_TRUE(used.count((uint32_t)p)) << titleRenderProgramName((uint32_t)p);
    }
    // progress rises monotonically to 1; every GPU step has a cost
    float last = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    while (!job->done() && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
        SystemRender::updateGpuCreation();
        RuntimeShaderCompiler::beginFrame();
        if (!job->waiting()) {
            EXPECT_GT(job->nextStepCostUs(), 0u);
        }
        job->step({0, 1});
        SystemRender::commit();
        ASSERT_FALSE(job->failed()) << job->error();
        EXPECT_GE(job->progress(), last);
        last = job->progress();
    }
    ASSERT_TRUE(job->done());
    EXPECT_FLOAT_EQ(job->progress(), 1.0f);
    EXPECT_EQ(job->passesDone(), job->passesTotal());
    EXPECT_EQ(job->nextStepCostUs(), 0u);
    const std::vector<NodeCost> costs = job->nodeCosts();
    ASSERT_FALSE(costs.empty());
    EXPECT_EQ(costs.front().layerId, "00000001");
    EXPECT_GT(costs.front().passes, 0u);

    // split into tiles of at least 256 x 256: the same image byte for byte
    Pixels whole = readImage(job->image());
    std::unique_ptr<TitleRenderJob> split = renderer->begin(requestOf(doc));
    const uint32_t before = split->passesTotal();
    split->splitOversizedSteps(1);
    EXPECT_GT(split->passesTotal(), before);
    ASSERT_TRUE(runJob(*split));
    Pixels tiled = readImage(split->image());
    EXPECT_TRUE(identical(whole, tiled)) << "a split pass drew outside its tile";
}

TEST_F(TitleArtRendererGpu, SlicedEqualsOneShot) {
    // the bake's one call and the preview's 1-pass slices after ten edits and their undo
    auto oneShotRenderer = makeRenderer();
    auto doc = classicDoc(640, 360);
    Pixels oneShot = render(*oneShotRenderer, requestOf(doc));

    auto previewRenderer = makeRenderer();
    std::shared_ptr<const TitleArtDoc> current = doc;
    for (int i = 0; i < 10; i++) {
        current = edited(current, [i](Json& root) {
            root["layers"][0]["style"]["rims"][i % 4]["width"] = 2.0 + i;
            root["layers"][0]["transform"]["position"] = Json::array({i * 3, -i * 2});
        });
        std::unique_ptr<TitleRenderJob> job = previewRenderer->begin(requestOf(current));
        ASSERT_TRUE(runJob(*job, {0, 1}));
    }
    std::unique_ptr<TitleRenderJob> undone = previewRenderer->begin(requestOf(doc));
    ASSERT_TRUE(runJob(*undone, {0, 1}));
    Pixels sliced = readImage(undone->image());
    EXPECT_TRUE(identical(oneShot, sliced));
}

TEST_F(TitleArtRendererGpu, RenderTwiceFreshIds) {
    auto renderer = makeRenderer();
    auto a = classicDoc(640, 360);
    // the same document with other ids everywhere: ids never reach pixels (R17)
    auto b = edited(a, [](Json& root) {
        Json& layer = root["layers"][0];
        layer["id"] = "7a7a7a7a";
        int n = 0;
        for (Json& r : layer["style"]["rims"]) r["id"] = "9" + std::to_string(1000000 + n++);
        for (Json& e : layer["effects"]) e["id"] = "8" + std::to_string(1000000 + n++);
    });
    Pixels first = render(*renderer, requestOf(a));
    auto fresh = makeRenderer();
    Pixels second = render(*fresh, requestOf(b));
    EXPECT_TRUE(identical(first, second));
}

TEST_F(TitleArtRendererGpu, CacheCascade) {
    auto renderer = makeRenderer();
    auto doc = classicDoc();
    Pixels full;
    const uint64_t fullPasses = passesOf(*renderer, requestOf(doc), &full);
    ASSERT_GT(fullPasses, 20u);
    RecordProperty("full_passes", (int)fullPasses);

    // a face colour change re-shades the stack, never the masks or distance fields
    auto recoloured = edited(doc, [](Json& root) { root["layers"][0]["style"]["face"]["bevel"]["highlight_color"] = "#ffe0e0"; });
    Pixels colour;
    const uint64_t colourPasses = passesOf(*renderer, requestOf(recoloured), &colour);
    RecordProperty("colour_passes", (int)colourPasses);
    // R42: the faces shade over the kept rims, the effects reuse what they drew from the
    // unchanged alpha (their blurs and fields): stack, placement, compose, output and one pass
    // per effect
    EXPECT_LE(colourPasses, 8u);
    auto freshRenderer = makeRenderer();
    EXPECT_TRUE(identical(colour, render(*freshRenderer, requestOf(recoloured)))) << "the cached colour change differs from a fresh render";

    // a whole-pixel move re-places the cached node: the compose and the output
    auto moved = edited(doc, [](Json& root) { root["layers"][0]["transform"]["position"] = Json::array({40, -24}); });
    Pixels move;
    render(*renderer, requestOf(doc));
    const uint64_t movePasses = passesOf(*renderer, requestOf(moved), &move);
    EXPECT_LE(movePasses, 2u);
    auto freshMove = makeRenderer();
    EXPECT_TRUE(identical(move, render(*freshMove, requestOf(moved)))) << "the re-placed node differs from a fresh render";

    // the outermost rim's width: only its own mask, field and the stack re-run
    auto wider = edited(doc, [](Json& root) { root["layers"][0]["style"]["rims"][3]["width"] = 3.5; });
    render(*renderer, requestOf(doc));
    Pixels rimImage;
    const uint64_t rimPasses = passesOf(*renderer, requestOf(wider), &rimImage);
    RecordProperty("rim_passes", (int)rimPasses);
    EXPECT_LE((double)rimPasses, 0.6 * (double)fullPasses);
    auto freshRim = makeRenderer();
    EXPECT_TRUE(identical(rimImage, render(*freshRim, requestOf(wider))));
    EXPECT_GT(renderer->stats().cacheHits, 0u);
    std::printf("[ cascade  ] full %llu, colour %llu, move %llu, outer rim %llu passes\n", (unsigned long long)fullPasses,
                (unsigned long long)colourPasses, (unsigned long long)movePasses, (unsigned long long)rimPasses);
}

TEST_F(TitleArtRendererGpu, RoiMemory) {
    auto renderer = makeRenderer();
    // the classic fixture at 1920 x 1080: R44's 96 MB
    std::unique_ptr<TitleRenderJob> job;
    render(*renderer, requestOf(classicDoc()), &job);
    const size_t peak = renderer->poolStats().peakBytes;
    RecordProperty("peak_bytes", (int)peak);
    std::printf("[ memory   ] classic fixture peak %.1f MB\n", peak / 1048576.0);
    EXPECT_LE(peak, (size_t)96 << 20);

    // a 20 px square in a corner: its node targets cover its region, not the canvas
    auto small = std::make_unique<TitleArtRenderer>();
    small->setContentProvider(std::make_shared<TestContent>());
    auto corner = makeDoc({{"canvas", canvas(1920, 1080)},
                           {"layers", Json::array({shapeLayer("00000001", "rect", 20, solidFace("#00ff00"),
                                                              {{"transform", {{"position", Json::array({-900, 500})}}}})})}});
    render(*small, requestOf(corner));
    const size_t canvasBytes = (size_t)1920 * 1080 * (colorFormatBytes(TitleArtCaps::select(SystemRender::getFeatures(), 0).format(TitleBuffer::Accumulator).format) + 4);
    // the canvas accumulator and the output, plus small targets only
    EXPECT_LE(small->poolStats().peakBytes, canvasBytes + ((size_t)1 << 20));
}

TEST_F(TitleArtRendererGpu, DeviceLoss) {
    auto renderer = makeRenderer();
    auto doc = classicDoc(640, 360);
    Pixels before = render(*renderer, requestOf(doc));
    // nothing cached: the job runs every pass
    renderer->trim();
    std::unique_ptr<TitleRenderJob> job = renderer->begin(requestOf(doc));
    // mid-render: a few steps, then the device goes
    for (int i = 0; i < 8 && !job->done(); i++) {
        SystemRender::updateGpuCreation();
        RuntimeShaderCompiler::beginFrame();
        job->step({0, 1});
        SystemRender::commit();
    }
    renderer->simulateDeviceLossForTesting();
    EXPECT_FALSE(runJob(*job, {}, 200));
    EXPECT_NE(job->error().find("device_lost"), std::string::npos) << job->error();
    EXPECT_EQ(renderer->cacheEntries(), 0u);
    // a new render after the loss starts from nothing and draws the same image
    Pixels after = render(*renderer, requestOf(doc));
    EXPECT_TRUE(identical(before, after));
}

TEST_F(TitleArtRendererGpu, OverridesEqualEdits) {
    auto renderer = makeRenderer();
    auto doc = classicDoc(640, 360);
    RenderRequest overridden = requestOf(doc);
    overridden.overrides.push_back({"00000001", "style.face.bevel.highlight_color", Json("#80ff80")});
    overridden.overrides.push_back({"00000001", "transform.position", Json::array({12, 8})});
    Pixels viaOverrides = render(*renderer, overridden);
    // the document itself is untouched
    EXPECT_EQ(doc->root["layers"][0]["transform"]["position"], Json::array({0, 0}));
    auto edit = edited(doc, [](Json& root) {
        root["layers"][0]["style"]["face"]["bevel"]["highlight_color"] = "#80ff80";
        root["layers"][0]["transform"]["position"] = Json::array({12, 8});
    });
    auto fresh = makeRenderer();
    EXPECT_TRUE(identical(viaOverrides, render(*fresh, requestOf(edit))));
}

TEST_F(TitleArtRendererGpu, KeptSplit) {
    auto renderer = makeRenderer();
    auto doc = makeDoc({{"canvas", canvas(320, 160)},
                        {"layers", Json::array({shapeLayer("00000001", "rect", 100, solidFace("#2040ff")),
                                                shapeLayer("00000002", "ellipse", 60, solidFace("#ff2020"),
                                                           {{"effects", Json::array({effect("00000003", "test_shadow")})}})})}});
    RenderRequest request = requestOf(doc);
    request.keepSplits = {"00000002"};
    request.output = TitleOutputFormat::Linear;
    std::unique_ptr<TitleRenderJob> job;
    render(*renderer, request, &job);
    ASSERT_TRUE(job->splits().layers.count("00000002"));
    const TitleImage& split = job->splits().layers.at("00000002");
    Pixels kept = readImage(split);
    // the layer alone (isolated) is the same composite
    RenderRequest alone = requestOf(doc);
    alone.isolateLayer = "00000002";
    alone.output = TitleOutputFormat::Linear;
    std::unique_ptr<TitleRenderJob> isolatedJob;
    Pixels isolated = render(*renderer, alone, &isolatedJob);
    // the split covers its node's region: compare it with that region of the isolated canvas
    const TitleRectI f = split.frame;
    ASSERT_GT(kept.width, 0);
    double worst = 0.0;
    for (int y = 0; y < kept.height; y++) {
        for (int x = 0; x < kept.width; x++) {
            const int cx = f.x + x, cy = f.y + y;
            if (cx < 0 || cy < 0 || cx >= isolated.width || cy >= isolated.height) continue;
            for (int c = 0; c < 4; c++) worst = std::max(worst, (double)std::abs(kept.at(x, y)[c] - isolated.at(cx, cy)[c]));
        }
    }
    EXPECT_LE(worst * 255.0, 1.0);
}

TEST_F(TitleArtRendererGpu, GlyphCell) {
    auto renderer = makeRenderer();
    Json layer{{"id", "00000001"}, {"type", "text"}, {"text", "FF"}, {"size", 80},
               {"style", {{"face", solidFace("#ffd000")}, {"rims", Json::array({rim("00000011", 6, "#301000", false)})}}},
               {"effects", Json::array({effect("00000021", "test_shadow")})}};
    auto doc = makeDoc({{"canvas", canvas(320, 160)}, {"layers", Json::array({layer})}});
    RenderRequest request = requestOf(doc);
    request.glyphCell = GlyphCellRequest{"00000001", 1, GlyphCellRequest::Back};
    request.output = TitleOutputFormat::Linear;
    std::unique_ptr<TitleRenderJob> job;
    Pixels merged = render(*renderer, request, &job);
    const TitleSplits& splits = job->splits();
    ASSERT_TRUE(splits.glyphBack.target);
    ASSERT_TRUE(splits.glyphFront.target);
    Pixels back = readImage(splits.glyphBack), front = readImage(splits.glyphFront);
    ASSERT_EQ(back.width, merged.width);
    // front over back is the cell's merged render
    double worst = 0.0;
    for (int y = 0; y < merged.height; y++) {
        for (int x = 0; x < merged.width; x++) {
            const float* b = back.at(x, y);
            const float* f = front.at(x, y);
            const float* m = merged.at(x, y);
            for (int c = 0; c < 4; c++) worst = std::max(worst, (double)std::abs(f[c] + b[c] * (1.0f - f[3]) - m[c]));
        }
    }
    EXPECT_LE(worst * 255.0, 1.0);
    // the cell holds the second glyph only: narrower than the whole word
    EXPECT_LT(merged.width, 320);
}

TEST_F(TitleArtRendererGpu, IdleIsFree) {
    auto renderer = makeRenderer();
    {
        Pixels p = render(*renderer, requestOf(classicDoc(320, 180)));
        ASSERT_GT(p.width, 0);
    }
    renderer->trim();
    const uint64_t passes = renderer->stats().passes;
    // nothing previewed or baking: no work and no targets (R49)
    for (int i = 0; i < 5; i++) {
        SystemRender::updateGpuCreation();
        SystemRender::commit();
    }
    EXPECT_EQ(renderer->stats().passes, passes);
    EXPECT_EQ(renderer->poolStats().bytesLive, 0u);
    EXPECT_EQ(renderer->poolStats().bytesIdle, 0u);
    EXPECT_EQ(renderer->poolStats().targets, 0u);
    EXPECT_EQ(renderer->cacheEntries(), 0u);
}

TEST_F(TitleArtRendererGpu, ClassicFixtureBudget) {
    // R41 on the test device: the classic-style fixture at 1920 x 1080 within llvmpipe's 8 s
    auto renderer = makeRenderer();
    auto doc = classicDoc();
    render(*renderer, requestOf(doc));     // the programs compile
    renderer->trim();
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<TitleRenderJob> job = renderer->begin(requestOf(doc));
    ASSERT_TRUE(runJob(*job));
    jasperengine::test::finishGpu();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    RecordProperty("full_render_ms", (int)ms);
    std::printf("[ budget   ] classic fixture full render %.0f ms (%u passes)\n", ms, job->passesTotal());
    EXPECT_LE(ms, 8000.0);
}

TEST_F(TitleArtRendererGpu, SixteenBitOutput) {
    // a 16-bit bake (RenderRequest::outputBitDepth): straight sRGB half floats, no dither, and
    // the target's colour texture readable by the editor (TitleTarget::colorTexture)
    if (!SystemRender::getFeatures().renderRGBA16F) GTEST_SKIP() << "this device renders no half floats";
    auto renderer = makeRenderer();
    auto doc = makeDoc({{"canvas", canvas(128, 64)}, {"layers", Json::array({shapeLayer("00000001", "rect", 32, solidFace("#ff8000"))})}});
    RenderRequest request = requestOf(doc);
    request.outputBitDepth = 16;
    std::unique_ptr<TitleRenderJob> job;
    Pixels p = render(*renderer, request, &job);
    ASSERT_EQ(job->image().format, ColorFormat::RGBA16F);
    ASSERT_NE(job->image().target.get(), nullptr);
    EXPECT_NE(job->image().target.get()->colorTexture(), nullptr);
    const float* centre = p.at(64, 32);
    EXPECT_NEAR(centre[0], 1.0f, 1e-3f);
    EXPECT_NEAR(centre[1], 128.0f / 255.0f, 2e-3f);
    EXPECT_NEAR(centre[2], 0.0f, 1e-3f);
    EXPECT_NEAR(centre[3], 1.0f, 1e-3f);
    EXPECT_EQ(p.at(0, 0)[3], 0.0f);
}

TEST_F(TitleArtRendererGpu, CanvasPositionsKeyTheNode) {
    // R14 with "what may move a cached node": a whole-pixel move re-places a layer whose effects
    // read only its own pixels, and re-renders one whose effect says it anchored to the canvas
    // (EffectContext::usesCanvasPosition, Pattern Overlay's `align: canvas`)
    auto docWith = [](const char* align, Json position) {
        Json layer = shapeLayer("00000001", "rect", 48, solidFace("#ffffff"),
                                Json{{"transform", {{"position", position}}},
                                     {"effects", Json::array({effect("00000021", "pattern_overlay",
                                                                     {{"pattern", "stripes"}, {"scale", 7}, {"angle", 30}, {"align", align}})})}});
        // a node that touches the canvas edge is never moved; this one stays clear of it
        return makeDoc({{"canvas", canvas(512, 384)}, {"layers", Json::array({layer})}});
    };
    for (const char* align : {"layer", "canvas"}) {
        SCOPED_TRACE(align);
        auto renderer = makeRenderer();
        render(*renderer, requestOf(docWith(align, Json::array({0, 0}))));
        auto moved = docWith(align, Json::array({5, -3}));
        Pixels move;
        const uint64_t passes = passesOf(*renderer, requestOf(moved), &move);
        auto fresh = makeRenderer();
        EXPECT_TRUE(identical(move, render(*fresh, requestOf(moved)))) << "the moved layer differs from a fresh render";
        if (std::string(align) == "layer") {
            EXPECT_LE(passes, 2u) << "a layer-anchored pattern moves with its cached node";
        } else {
            EXPECT_GT(passes, 2u) << "a canvas-anchored pattern must render again where it moved";
        }
    }
}

TEST_F(TitleArtRendererGpu, GeneratorGetsItsFollowedLayer) {
    // a following generator (FollowMode Optional) is handed the layer `follow` names, rendered
    // alone, with its own target's key: this one draws its silhouette in green over it
    GeneratorDef d;
    d.type = "test_follow";
    d.label = "Test Follow";
    d.category = "Test";
    d.follow = FollowMode::Optional;
    d.followReach = [](const ParamValues&) { return 0.0f; };
    d.render = [](GeneratorContext& ctx, TargetRef& target, const ParamValues&) {
        ctx.runner.clear(target);
        if (!ctx.follow) return;
        EXPECT_EQ(ctx.follow->key(), target.key());
        EffectContext e(ctx.runner);
        e.width = ctx.width;
        e.height = ctx.height;
        e.pxScale = ctx.pxScale;
        e.frame = ctx.frame;
        e.documentToTarget = ctx.documentToTarget;
        const Json overlay{{"color", "#00ff00"}, {"opacity", 1.0}, {"mode", "normal"}};
        findEffect("color_overlay")->apply(e, *ctx.follow, target, ParamValues(overlay));
    };
    ASSERT_TRUE(TitleEffectRegistry::get().add(d));
    auto docFollowing = [](const char* followed) {
        Json gen{{"id", "00000002"}, {"type", "generator"}, {"name", "Follow"}, {"follow", followed},
                 {"generator", {{"type", "test_follow"}, {"params", Json::object()}}}};
        return makeDoc({{"canvas", canvas(256, 128)},
                        {"layers", Json::array({shapeLayer("00000001", "rect", 48, solidFace("#ff0000")), gen})}});
    };
    auto renderer = makeRenderer();
    std::unique_ptr<TitleRenderJob> job;
    Pixels followed = render(*renderer, requestOf(docFollowing("00000001")), &job);
    Pixels alone = render(*renderer, requestOf(docFollowing("")));
    Pixels itself = render(*renderer, requestOf(docFollowing("00000002")), &job);
    TitleEffectRegistry::get().removeGenerator("test_follow");
    ASSERT_FALSE(followed.bytes.empty());
    // the rect's centre: green where the generator follows it, the rect's red where it follows nothing
    const uint8_t* g = &followed.bytes[((size_t)64 * 256 + 128) * 4];
    EXPECT_EQ(g[0], 0);
    EXPECT_EQ(g[1], 255);
    EXPECT_EQ(g[3], 255);
    const uint8_t* r = &alone.bytes[((size_t)64 * 256 + 128) * 4];
    EXPECT_EQ(r[0], 255);
    EXPECT_EQ(r[1], 0);
    // outside the rect the generator draws nothing
    EXPECT_EQ(followed.bytes[((size_t)8 * 256 + 8) * 4 + 3], 0);
    // a generator following itself is a cycle: reported, drawn as following nothing
    EXPECT_TRUE(identical(itself, alone));
    bool cycle = false;
    for (const TitleProblem& p : job->problems()) cycle = cycle || p.code == "follow_cycle";
    EXPECT_TRUE(cycle) << "follow_cycle is reported";
}

TEST_F(TitleArtRendererGpu, IsolationKeysTheGroupNode) {
    // R14 with isolation: a group drawn for one isolated child holds only that child, so its node
    // must not share a cache entry with the whole group (a check's isolated render after the full
    // one drew both children; the full render after an isolated one drew one)
    // the red ellipse lies inside the blue square: the group covers the same region either way
    Json group{{"id", "00000001"}, {"type", "group"},
               {"children", Json::array({shapeLayer("00000002", "rect", 80, solidFace("#2040ff")), shapeLayer("00000003", "ellipse", 30, solidFace("#ff2020"))})}};
    auto doc = makeDoc({{"canvas", canvas(320, 160)}, {"layers", Json::array({group})}});
    RenderRequest whole = requestOf(doc);
    RenderRequest alone = requestOf(doc);
    alone.isolateLayer = "00000002";
    auto freshWhole = makeRenderer();
    auto freshAlone = makeRenderer();
    const Pixels expectWhole = render(*freshWhole, whole), expectAlone = render(*freshAlone, alone);
    ASSERT_FALSE(identical(expectWhole, expectAlone));
    {
        auto renderer = makeRenderer();
        render(*renderer, whole);
        EXPECT_TRUE(identical(render(*renderer, alone), expectAlone)) << "the isolated child after the whole group draws the group's other child";
    }
    {
        auto renderer = makeRenderer();
        render(*renderer, alone);
        EXPECT_TRUE(identical(render(*renderer, whole), expectWhole)) << "the whole group after an isolated child draws that child alone";
    }
}
