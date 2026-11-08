// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC21: Pachislop's 40 3D looks against its references. Each look (transcribed as JSON:
// titleart/fixtures/pachislop3d, by SafEight, used with the author's permission) is converted by
// title3DFromPachislop (R67) onto "BIG" in Dela Gothic One at size 200 on the 1920 x 1080 canvas,
// with its effects mapped onto this program's (keyline -> outline, outerGlow -> outer_glow; an
// effect this branch has not got is left out and named), rendered at 0.25 and trimmed at alpha 8
// (Pachislop's harness), centred on the reference (golden/titleart/pachislop/look/, copied
// unchanged) and compared premultiplied: size within 2 px, mean at most 6/255, at most 4 % of
// pixels over 24/255. The looks that cannot pass here are listed with their cause; every other
// look must pass, and a listed look that passes is reported so the list shrinks. Red control:
// Punch replaced by Neutral fails 20 looks or more.

#include "titleart/render/TitleRenderTestSupport.h"

#include "titleart/support/TitleTestAssets.h"
#include "titleart/three_d/Title3DConvert.h"

#include "ImageCompare.h"
#include "TestPaths.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    struct Image8 {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;      // straight sRGB
    };

    Image8 trim(const Image8& image, int threshold) {
        int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                if (image.rgba[((size_t)y * image.width + x) * 4 + 3] >= threshold) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
        }
        Image8 out;
        if (x1 < 0) return out;
        out.width = x1 - x0 + 1;
        out.height = y1 - y0 + 1;
        out.rgba.resize((size_t)out.width * out.height * 4);
        for (int y = 0; y < out.height; y++) {
            std::copy_n(&image.rgba[((size_t)(y + y0) * image.width + x0) * 4], (size_t)out.width * 4, &out.rgba[(size_t)y * out.width * 4]);
        }
        return out;
    }

    Image8 centred(const Image8& image, int width, int height) {
        Image8 out;
        out.width = width;
        out.height = height;
        out.rgba.assign((size_t)width * height * 4, 0);
        const int ox = (int)std::floor((width - image.width) / 2.0), oy = (int)std::floor((height - image.height) / 2.0);
        for (int y = 0; y < image.height; y++) {
            const int ty = y + oy;
            if (ty < 0 || ty >= height) continue;
            for (int x = 0; x < image.width; x++) {
                const int tx = x + ox;
                if (tx < 0 || tx >= width) continue;
                std::copy_n(&image.rgba[((size_t)y * image.width + x) * 4], 4, &out.rgba[((size_t)ty * width + tx) * 4]);
            }
        }
        return out;
    }

    struct Parity {
        int dw = 0, dh = 0;
        double mean = 0, over = 0;
        bool pass() const { return std::abs(dw) <= 2 && std::abs(dh) <= 2 && mean <= 6.0 && over <= 0.04; }
        std::string text() const {
            char t[160];
            std::snprintf(t, sizeof(t), "size %+d x %+d px, mean %.2f/255, %.2f %% of px over 24/255", dw, dh, mean, over * 100.0);
            return t;
        }
    };

    // AC21's metric on premultiplied 8-bit values, ours centred on the reference
    Parity compareLook(const Image8& ref, const Image8& rendered) {
        Parity p;
        const Image8 trimmed = trim(rendered, 8);
        p.dw = trimmed.width - ref.width;
        p.dh = trimmed.height - ref.height;
        const Image8 ours = centred(trimmed, ref.width, ref.height);
        const size_t n = (size_t)ref.width * ref.height;
        double sum = 0;
        size_t over = 0;
        for (size_t i = 0; i < n; i++) {
            double worst = 0;
            for (int c = 0; c < 4; c++) {
                const double a = c == 3 ? ref.rgba[i * 4 + 3] : ref.rgba[i * 4 + c] * ref.rgba[i * 4 + 3] / 255.0;
                const double b = c == 3 ? ours.rgba[i * 4 + 3] : ours.rgba[i * 4 + c] * ours.rgba[i * 4 + 3] / 255.0;
                const double d = std::abs(a - b);
                sum += d;
                worst = std::max(worst, d);
            }
            if (worst > 24) over++;
        }
        p.mean = n ? sum / (double)(n * 4) : 255;
        p.over = n ? (double)over / (double)n : 1;
        return p;
    }

    std::string snake(const std::string& camel) {
        std::string out;
        for (char c : camel) {
            if (std::isupper((unsigned char)c)) {
                out += '_';
                out += (char)std::tolower((unsigned char)c);
            } else {
                out += c;
            }
        }
        return out;
    }

    // a look's effects as this program's: the types it has, params snake_cased; the others named
    Json effectsOf(const Json& effects, std::vector<std::string>& missing) {
        static const std::map<std::string, std::string> TYPES = {{"keyline", "outline"}, {"outerGlow", "outer_glow"}, {"innerGlow", "inner_glow"},
                                                                 {"dropShadow", "drop_shadow"}, {"innerShadow", "inner_shadow"}};
        Json out = Json::array();
        int n = 0;
        for (const Json& e : effects) {
            const std::string type = e.value("type", std::string());
            auto it = TYPES.find(type);
            if (it == TYPES.end()) {
                missing.push_back(type);
                continue;
            }
            Json params = Json::object();
            for (auto p = e["params"].begin(); p != e["params"].end(); ++p) params[snake(p.key())] = p.value();
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", 0x300 + n++);
            out.push_back(Json{{"id", id}, {"type", it->second}, {"enabled", e.value("enabled", true)}, {"params", params}});
        }
        return out;
    }

    // the cause of each look that cannot pass in this branch (filled from the measured runs)
    const std::map<std::string, std::string>& exceptions() {
        static const std::map<std::string, std::string> list = {};
        return list;
    }

    class Title3DParityGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }

        // a look on BIG, rendered at 0.25 (straight sRGB)
        Image8 renderLook(const Json& look, bool neutral, std::vector<std::string>& missing) {
            static TestAssetResolver assets;
            Json style3d = title3DFromPachislop(look["style3d"]);
            if (neutral && style3d.contains("lighting")) style3d["lighting"]["tone_mapping"] = "neutral";
            Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "BIG"}, {"text", "BIG"}, {"font", fontRef(DELA_GOTHIC)},
                              {"size", 200}, {"dimension", "3d"}, {"style_3d", style3d},
                              {"effects", effectsOf(look.contains("effects") ? look["effects"] : Json::array(), missing)}};
            auto doc = makeDoc({{"canvas", canvas(1920, 1080)}, {"layers", Json::array({layer})}});
            // one renderer for every look: its environment cubes are built once
            if (!renderer) {
                renderer = std::make_unique<TitleArtRenderer>();
                renderer->setContentProvider(makeGeometryTitleContentProvider());
            }
            RenderRequest r;
            r.doc = doc;
            r.pxScale = 0.25f;
            r.assets = &assets;
            const Pixels p = render(*renderer, r);
            Image8 out;
            out.width = p.width;
            out.height = p.height;
            out.rgba = p.bytes;
            if (out.rgba.empty()) {
                for (float v : p.rgba) out.rgba.push_back((uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            return out;
        }

        std::unique_ptr<TitleArtRenderer> renderer;
    };

}

TEST_F(Title3DParityGpu, PachislopLooks) {
    const std::filesystem::path fixtures = std::filesystem::path(jasperengine::test::testPaths().sourceDir) / "titleart" / "fixtures" / "pachislop3d";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(fixtures)) {
        if (entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    ASSERT_EQ(files.size(), 40u);
    int passed = 0, neutralFailed = 0;
    std::set<std::string> failing;
    for (const auto& file : files) {
        std::ifstream in(file);
        const Json look = Json::parse(in, nullptr, false);
        ASSERT_TRUE(look.is_object()) << file;
        const std::string id = look.value("id", std::string());
        SCOPED_TRACE(id);
        Image8 ref;
        ASSERT_TRUE(jasperengine::test::readPng(jasperengine::test::getGoldenDirectory() + "/titleart/pachislop/look/" + id + ".png", ref.rgba,
                                                ref.width, ref.height))
            << id;
        std::vector<std::string> missing;
        const Image8 ours = renderLook(look, false, missing);
        ASSERT_GT(ours.width, 0);
        const Parity p = compareLook(ref, ours);
        std::string note;
        for (const std::string& m : missing) note += (note.empty() ? " (left out: " : ", ") + m;
        if (!note.empty()) note += ")";
        std::printf("[parity3d] %s: %s %s%s\n", id.c_str(), p.text().c_str(), p.pass() ? "PASS" : "FAIL", note.c_str());
        if (p.pass()) passed++;
        else failing.insert(id);
        if (!p.pass() || jasperengine::test::isGoldenUpdateMode()) {
            const Image8 trimmed = trim(ours, 8);
            const std::filesystem::path actual = std::filesystem::path(jasperengine::test::getOutputDirectory()) / ("title3d-parity-" + id + ".png");
            std::filesystem::create_directories(actual.parent_path());
            if (trimmed.width > 0) jasperengine::test::writePng(actual.string(), trimmed.rgba, trimmed.width, trimmed.height);
        }
        // red control: Neutral instead of Punch
        std::vector<std::string> ignored;
        if (!compareLook(ref, renderLook(look, true, ignored)).pass()) neutralFailed++;
    }
    std::printf("[parity3d] %d of %zu looks pass; Neutral for Punch fails %d\n", passed, files.size(), neutralFailed);
    for (const std::string& id : failing) {
        EXPECT_TRUE(exceptions().count(id)) << id << " fails and has no recorded cause";
    }
    for (const auto& [id, cause] : exceptions()) {
        EXPECT_TRUE(failing.count(id)) << id << " passes now: take it off the exceptions (" << cause << ")";
    }
    EXPECT_GE(neutralFailed, 20) << "the metric tells Punch from Neutral";
}
