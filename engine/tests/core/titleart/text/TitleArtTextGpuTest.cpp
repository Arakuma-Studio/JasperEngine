// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Text and element masks through the whole renderer (docs/specs/S80-title-art-core.md R20-R26,
// AC21, AC22): the module's own text and geometry (TitleGeometryContent: TitleTextLayout and
// TitleGeometry behind the renderer's 2D passes) with the OFL test fonts, against goldens written
// on glcore (JASPER_TEST_UPDATE_GOLDEN=1; Vulkan matches).
// - TitleArtTextGpu.Vertical: 縦書き with a tate-chu-yoko run in Dela Gothic One (vert forms).
// - TitleArtTextGpu.Variable: a CFF2 variable face (Source Sans 3 VF) at wght 400 and 800, which
//   opens only with FontFaceRasterizer::Optional; the two differ.
// - TitleArtTextGpu.Standard: the standard title of §7 (確定, the classic style).
// - TitleArtMaskGpu.RimJoins: a star with a mitred and a round rim; the joins differ.
// - TitleArtTextGpu.ClassicFixture: §7's classic fixture's budgets and caches on real text.

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleStandardTitle.h"

#include "ImageCompare.h"

#include <chrono>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    class TitleArtTextGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }

        // `doc` through a renderer with the module's own content provider and the test fonts
        Pixels renderWithText(const TitleArtDoc& doc, std::vector<TitleProblem>* problems = nullptr) {
            TitleArtRenderer renderer;
            RenderRequest request;
            request.doc = std::make_shared<const TitleArtDoc>(doc);
            request.assets = &assets;
            std::unique_ptr<TitleRenderJob> job;
            Pixels p = render(renderer, request, &job);
            if (job && problems) *problems = job->problems();
            return p;
        }

        void expectGolden(const std::string& name, const Pixels& p) {
            const jasperengine::test::CompareResult r = jasperengine::test::compareWithGolden(
                "titleart/text/" + name, p.bytes, p.width, p.height, jasperengine::test::hardwareTolerance({24, 0.01, 1.0, 4, 6.0}, {}));
            EXPECT_TRUE(r.passed) << name << ": " << r.message;
        }

        TestAssetResolver assets;
    };

    class TitleArtMaskGpu : public TitleArtTextGpu {};

    Json textLayer(const char* id, const std::string& text, const char* font, double size, Json extra = Json::object()) {
        Json layer{{"id", id}, {"type", "text"}, {"name", id}, {"text", text}, {"font", fontRef(font)}, {"size", size},
                   {"style", {{"face", solidFace("#ffffff")}, {"rims", Json::array({{{"id", "00000101"}, {"width", 4}, {"material", solidFace("#c02020")}}})}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) layer[it.key()] = it.value();
        return layer;
    }

    TitleArtDoc docOf(int w, int h, const std::vector<Json>& layers) {
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(w, h);
        root["layers"] = Json::array();
        for (const Json& l : layers) root["layers"].push_back(l);
        return parseTitleArtJson(std::move(root)).doc;
    }

    // the share of pixels whose alpha differs by more than 8/255
    double alphaDifference(const Pixels& a, const Pixels& b) {
        if (a.bytes.size() != b.bytes.size() || a.bytes.empty()) return 1.0;
        size_t differ = 0;
        for (size_t i = 3; i < a.bytes.size(); i += 4) differ += std::abs((int)a.bytes[i] - (int)b.bytes[i]) > 8 ? 1 : 0;
        return (double)differ / (double)(a.bytes.size() / 4);
    }

    bool drewSomething(const Pixels& p) {
        for (size_t i = 3; i < p.bytes.size(); i += 4) {
            if (p.bytes[i] > 0) return true;
        }
        return false;
    }

    bool hasProblem(const std::vector<TitleProblem>& problems, const char* code) {
        for (const TitleProblem& p : problems) {
            if (p.code == code) return true;
        }
        return false;
    }

}

TEST_F(TitleArtTextGpu, Vertical) {
    std::vector<TitleProblem> problems;
    const Pixels p = renderWithText(docOf(256, 512, {textLayer("00000001", "縦書き12。", DELA_GOTHIC, 80, Json{{"vertical", true}})}), &problems);
    ASSERT_TRUE(drewSomething(p));
    EXPECT_FALSE(hasProblem(problems, "glyph_missing"));
    EXPECT_FALSE(hasProblem(problems, "geometry_unavailable"));
    // a column: far taller than it is wide
    int x0 = p.width, x1 = -1, y0 = p.height, y1 = -1;
    for (int y = 0; y < p.height; y++) {
        for (int x = 0; x < p.width; x++) {
            if (p.bytes[((size_t)y * p.width + x) * 4 + 3] < 128) continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    EXPECT_GT(y1 - y0, 3 * (x1 - x0)) << "the text runs down one column";
    expectGolden("vertical", p);
}

TEST_F(TitleArtTextGpu, Variable) {
    auto weighted = [&](int wght) {
        return renderWithText(docOf(512, 160, {textLayer("00000001", "Bold", SOURCE_SANS_VF, 110, Json{{"variations", {{"wght", wght}}}})}));
    };
    const Pixels regular = weighted(400), heavy = weighted(800);
    ASSERT_TRUE(drewSomething(regular)) << "the CFF2 face opened (FontFaceRasterizer::Optional) and drew";
    ASSERT_TRUE(drewSomething(heavy));
    size_t inkRegular = 0, inkHeavy = 0;
    for (size_t i = 3; i < regular.bytes.size(); i += 4) {
        inkRegular += regular.bytes[i] >= 128 ? 1 : 0;
        inkHeavy += heavy.bytes[i] >= 128 ? 1 : 0;
    }
    EXPECT_GT((double)inkHeavy, 1.2 * (double)inkRegular) << "wght 800 draws heavier strokes than 400";
    expectGolden("variable_400", regular);
    expectGolden("variable_800", heavy);
}

TEST_F(TitleArtTextGpu, Standard) {
    std::vector<TitleProblem> problems;
    TitleArtDoc doc = standardTitleDocument();
    // the canvas at a third of Pachislop's, so the golden stays small
    doc.root["canvas"]["width"] = 640;
    doc.root["canvas"]["height"] = 360;
    const Pixels p = renderWithText(doc, &problems);
    ASSERT_TRUE(drewSomething(p));
    EXPECT_FALSE(hasProblem(problems, "glyph_missing"));
    expectGolden("standard", p);
}

TEST_F(TitleArtMaskGpu, RimJoins) {
    auto star = [&](const char* join) {
        Json layer = shapeLayer("00000001", "star", 180, solidFace("#ffd040"),
                                Json{{"points", 5}, {"inner", 0.4}});
        layer["style"]["rims"] = Json::array({{{"id", "00000101"}, {"width", 8}, {"join", join}, {"material", solidFace("#202020")}}});
        return renderWithText(docOf(320, 320, {layer}));
    };
    const Pixels miter = star("miter"), round = star("round");
    ASSERT_TRUE(drewSomething(miter));
    EXPECT_GT(alphaDifference(miter, round), 0.002) << "mitred and round rims differ at the star's points";
    expectGolden("rim_miter", miter);
    expectGolden("rim_round", round);
}

// §7's classic fixture itself (the standard title with Outer Glow and Drop Shadow, real text and
// effects) at 1920 x 1080, scale 1: R44's peak target memory (AC14), R43's flood passes against
// unbounded fields (AC23), R41's llvmpipe wall time (AC29's software lane), R42's whole-pixel
// move (AC15) and cached results equal to fresh ones; the face colour change's count is recorded
// (Deviation 40: the layer effects re-run on it)
TEST_F(TitleArtTextGpu, ClassicFixture) {
    TitleArtRenderer renderer;
    RenderRequest request;
    request.doc = std::make_shared<const TitleArtDoc>(classicFixtureDocument());
    request.assets = &assets;
    std::unique_ptr<TitleRenderJob> job;
    const Pixels first = render(renderer, request, &job);
    ASSERT_TRUE(drewSomething(first));
    for (const TitleProblem& p : job->problems()) ADD_FAILURE() << p.code << ": " << p.message;
    const size_t peak = renderer.poolStats().peakBytes;
    const RendererStats stats = renderer.stats();
    EXPECT_LE(peak, (size_t)96 << 20);
    EXPECT_GT(stats.floodPasses, 0u);
    EXPECT_LE((double)stats.floodPasses, 0.5 * (double)stats.floodPassesUnbounded);

    // a full render on warm programs and an empty cache
    renderer.trim();
    const uint64_t before = renderer.stats().passes;
    const auto start = std::chrono::steady_clock::now();
    job = renderer.begin(request);
    ASSERT_TRUE(runJob(*job));
    jasperengine::test::finishGpu();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const uint64_t fullPasses = renderer.stats().passes - before;
    EXPECT_LE(ms, 8000.0);

    auto variant = [&](const std::function<void(Json&)>& edit) {
        RenderRequest r = request;
        Json root = request.doc->root;
        edit(root);
        r.doc = std::make_shared<const TitleArtDoc>(parseTitleArtJson(std::move(root)).doc);
        return r;
    };
    auto fresh = [&](const RenderRequest& r) {
        TitleArtRenderer other;
        return render(other, r);
    };
    // a whole-pixel move re-places the cached node
    const RenderRequest moved = variant([](Json& root) { root["layers"][0]["transform"]["position"] = Json::array({40, -24}); });
    uint64_t at = renderer.stats().passes;
    const Pixels move = render(renderer, moved);
    const uint64_t movePasses = renderer.stats().passes - at;
    EXPECT_LE(movePasses, 2u);
    EXPECT_TRUE(identical(move, fresh(moved))) << "the re-placed node differs from a fresh render";
    // a face colour change keeps masks and fields
    const RenderRequest recoloured = variant([](Json& root) { root["layers"][0]["style"]["face"]["bevel"]["highlight_color"] = "#ffe0e0"; });
    render(renderer, request);
    at = renderer.stats().passes;
    const Pixels colour = render(renderer, recoloured);
    const uint64_t colourPasses = renderer.stats().passes - at;
    EXPECT_LT(colourPasses, fullPasses);
    EXPECT_TRUE(identical(colour, fresh(recoloured))) << "the cached colour change differs from a fresh render";
    std::printf("[ classic  ] peak %.1f MB, floods %llu of %llu unbounded, full %.0f ms %llu passes, move %llu, colour %llu\n",
                peak / 1048576.0, (unsigned long long)stats.floodPasses, (unsigned long long)stats.floodPassesUnbounded, ms,
                (unsigned long long)fullPasses, (unsigned long long)movePasses, (unsigned long long)colourPasses);
}
