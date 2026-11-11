// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Blending (docs/specs/S80-title-art-core.md R15, §6.4; AC16, AC44): each of the 18 modes on a
// fixed pair of layers against a CPU reference of titleart/render/blend.glsl's maths within 2/255;
// Normal and Add drawn by fixed-function blending (no backdrop copy); and the light and max
// blend modes the renderer added to the render layer, on RGBA8 and RGBA16F targets.

#include "TitleRenderTestSupport.h"

#include "titleart/render/TitlePrograms.h"

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    using V3 = std::array<double, 3>;
    using V4 = std::array<double, 4>;

    double lum(const V3& c) { return c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722; }

    // blend.glsl's taBlendFn, on linear straight colour
    V3 blendFn(V3 b, V3 o, int mode) {
        const bool keeps = mode == 0 || mode == 1 || mode == 3 || mode == 7 || mode == 8 || mode == 12;
        if (!keeps) {
            for (int i = 0; i < 3; i++) {
                b[i] = std::clamp(b[i], 0.0, 1.0);
                o[i] = std::clamp(o[i], 0.0, 1.0);
            }
        }
        V3 r = o;
        for (int i = 0; i < 3; i++) {
            const double bb = b[i], oo = o[i];
            switch (mode) {
                case 1: r[i] = bb + oo; break;
                case 2: r[i] = 1 - (1 - bb) * (1 - oo); break;
                case 3: r[i] = bb * oo; break;
                case 4: r[i] = bb < 0.5 ? 2 * bb * oo : 1 - 2 * (1 - bb) * (1 - oo); break;
                case 5: r[i] = oo < 0.5 ? 2 * bb * oo + bb * bb * (1 - 2 * oo) : 2 * bb * (1 - oo) + std::sqrt(std::max(bb, 0.0)) * (2 * oo - 1); break;
                case 6: r[i] = oo < 0.5 ? 2 * bb * oo : 1 - 2 * (1 - bb) * (1 - oo); break;
                case 7: r[i] = std::min(bb, oo); break;
                case 8: r[i] = std::max(bb, oo); break;
                case 9: r[i] = (1 - oo) < 0.001 ? bb : std::min(bb / std::max(1 - oo, 1e-6), 1.0); break;
                case 10: r[i] = oo < 0.0001 ? 0.0 : std::max(1 - (1 - bb) / (oo + 0.0001), 0.0); break;
                case 11: r[i] = std::max(bb + oo - 1, 0.0); break;
                case 12: r[i] = std::abs(bb - oo); break;
                case 13: r[i] = bb + oo - 2 * bb * oo; break;
                default: break;
            }
        }
        if (mode >= 14) {
            const double bl = lum(b), ol = lum(o);
            if (mode == 14 || mode == 15) {
                const V3& chromaOf = mode == 14 ? o : b;
                const double cl = mode == 14 ? ol : bl;
                V3 c{chromaOf[0] - cl, chromaOf[1] - cl, chromaOf[2] - cl};
                const double len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                const V3& sized = mode == 14 ? b : o;
                const double sl = mode == 14 ? bl : ol;
                const double slen = std::sqrt(std::pow(sized[0] - sl, 2) + std::pow(sized[1] - sl, 2) + std::pow(sized[2] - sl, 2));
                for (int i = 0; i < 3; i++) r[i] = (len > 0.0001 ? c[i] / len : 0.0) * slen + bl;
            } else if (mode == 16) {
                for (int i = 0; i < 3; i++) r[i] = o[i] + (bl - ol);
            } else {
                for (int i = 0; i < 3; i++) r[i] = b[i] * (ol / (bl + 0.0001));
            }
        }
        return r;
    }

    V4 blend(const V4& d, const V4& s, int mode) {
        if (mode == 0) return {s[0] + d[0] * (1 - s[3]), s[1] + d[1] * (1 - s[3]), s[2] + d[2] * (1 - s[3]), s[3] + d[3] * (1 - s[3])};
        const double oa = s[3] + d[3] - s[3] * d[3];
        if (mode == 1) return {d[0] + s[0], d[1] + s[1], d[2] + s[2], oa};
        V3 cb{d[0] / d[3], d[1] / d[3], d[2] / d[3]}, cs{s[0] / s[3], s[1] / s[3], s[2] / s[3]};
        const V3 f = blendFn(cb, cs, mode);
        V4 o;
        for (int i = 0; i < 3; i++) o[i] = s[i] * (1 - d[3]) + d[i] * (1 - s[3]) + s[3] * d[3] * f[i];
        o[3] = oa;
        return o;
    }

    double toSrgb(double c) {
        c = std::clamp(c, 0.0, 1.0);
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

    class TitleArtBlendGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

    std::shared_ptr<const TitleArtDoc> pairDoc(const char* mode) {
        Json below = shapeLayer("00000001", "rect", 64, Json{{"fill", {{"kind", "solid"}, {"color", "#40a0e0"}}}, {"opacity", 0.7}});
        Json above = shapeLayer("00000002", "rect", 48, solidFace("#e06030"), {{"blend", mode}, {"opacity", 0.8}});
        return makeDoc({{"canvas", canvas(128, 128)}, {"layers", Json::array({below, above})}});
    }

}

TEST_F(TitleArtBlendGpu, AllModes) {
    const V3 below{srgbToLinear(0x40 / 255.0f), srgbToLinear(0xa0 / 255.0f), srgbToLinear(0xe0 / 255.0f)};
    const V3 above{srgbToLinear(0xe0 / 255.0f), srgbToLinear(0x60 / 255.0f), srgbToLinear(0x30 / 255.0f)};
    const V4 d{below[0] * 0.7, below[1] * 0.7, below[2] * 0.7, 0.7};
    const V4 s{above[0] * 0.8, above[1] * 0.8, above[2] * 0.8, 0.8};
    for (int mode = 0; mode < TITLE_BLEND_COUNT; mode++) {
        const char* name = titleBlendName((TitleBlend)mode);
        auto renderer = makeRenderer();
        RenderRequest request;
        request.doc = pairDoc(name);
        request.output = TitleOutputFormat::PremultipliedSrgb8;
        Pixels p = render(*renderer, request);
        ASSERT_EQ(p.width, 128) << name;
        const V4 o = blend(d, s, mode);
        const uint8_t* centre = &p.bytes[((size_t)64 * 128 + 64) * 4];
        const double a = std::clamp(o[3], 0.0, 1.0);
        for (int c = 0; c < 3; c++) {
            const double expected = toSrgb(a > 0 ? o[c] / a : 0.0) * a * 255.0;
            EXPECT_NEAR(centre[c], expected, 2.0) << name << " channel " << c;
        }
        EXPECT_NEAR(centre[3], a * 255.0, 2.0) << name;
    }
}

TEST_F(TitleArtBlendGpu, FixedFunctionPaths) {
    // Normal and Add blend into the parent in fixed function: one pass less than a mode that
    // copies its backdrop first
    auto passesOf = [](const char* mode) {
        auto renderer = makeRenderer();
        RenderRequest request;
        request.doc = pairDoc(mode);
        std::unique_ptr<TitleRenderJob> job = renderer->begin(request);
        return job->passesTotal();
    };
    const bool fixed = !TitleArtCaps::select(SystemRender::getFeatures(), 0).floatPath || TitleArtCaps::select(SystemRender::getFeatures(), 0).floatBlend;
    ASSERT_TRUE(fixed) << "this device blends no RGBA16F: Normal takes the shader path";
    EXPECT_EQ(passesOf("normal") + 1, passesOf("multiply"));
    EXPECT_EQ(passesOf("add") + 1, passesOf("screen"));
    EXPECT_EQ(passesOf("normal"), passesOf("add"));
}

TEST(BlendModesGpu, LightAndMax) {
    JASPER_REQUIRE_GPU();
    TitleGpu gpu;
    SokolTitleTargetAllocator allocator;
    TitleTargetPool pool(allocator, 64u << 20);
    // premultiplied source and destination texels
    const float dst[4] = {0.2f, 0.4f, 0.1f, 0.5f}, src[4] = {0.3f, 0.1f, 0.5f, 0.6f};
    auto upload = [](const float v[4]) {
        auto u = std::make_unique<TitleUpload>();
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 4; i++) {
            for (int c = 0; c < 4; c++) bytes.push_back((uint8_t)std::lround(v[c] * 255.0f));
        }
        EXPECT_TRUE(u->create("blend test", 2, 2, ColorFormat::RGBA, bytes.data(), bytes.size(), TextureFilter::NEAREST));
        return u;
    };
    auto d = upload(dst), s = upload(src);
    const float q = 1.0f / 255.0f;
    auto quant = [q](float v) { return std::round(v * 255.0f) * q; };
    struct Case {
        BlendMode mode;
        float expected[4];
    };
    const float dq[4] = {quant(dst[0]), quant(dst[1]), quant(dst[2]), quant(dst[3])}, sq[4] = {quant(src[0]), quant(src[1]), quant(src[2]), quant(src[3])};
    const Case cases[] = {
        {BlendMode::LIGHT_PREMULTIPLIED, {dq[0] + sq[0], dq[1] + sq[1], dq[2] + sq[2], sq[3] + dq[3] * (1.0f - sq[3])}},
        {BlendMode::MAX_PREMULTIPLIED, {std::max(dq[0], sq[0]), std::max(dq[1], sq[1]), std::max(dq[2], sq[2]), std::max(dq[3], sq[3])}},
    };
    for (ColorFormat format : {ColorFormat::RGBA, ColorFormat::RGBA16F}) {
        if (format == ColorFormat::RGBA16F && !(SystemRender::getFeatures().renderRGBA16F && SystemRender::getFeatures().blendRGBA16F)) continue;
        for (const Case& c : cases) {
            TargetKey key;
            key.width = key.height = 2;
            key.format = format;
            key.filter = TextureFilter::NEAREST;
            TargetRef target = pool.acquire(key);
            ASSERT_TRUE(target);
            std::string error;
            for (int step = 0; step < 2; step++) {
                TitleGpuPass p;
                p.program = TitleRenderProgram::Copy;
                p.target = target.get();
                p.tex[0] = step == 0 ? &d->texture() : &s->texture();
                p.params.set(2, 0.5f, 0.5f, 0.0f, 0.0f);
                p.params.set(3, 1.0f);
                p.blend = step == 0 ? BlendMode::NONE : c.mode;
                bool ran = false;
                for (int frame = 0; frame < 2000 && !ran; frame++) {
                    SystemRender::updateGpuCreation();
                    RuntimeShaderCompiler::beginFrame();
                    if (gpu.ensureProgram(TitleRenderProgram::Copy, &error)) ran = gpu.run(p, error);
                    SystemRender::commit();
                    ASSERT_TRUE(error.empty()) << error;
                }
                ASSERT_TRUE(ran);
            }
            TitleImage image;
            image.target = std::move(target);
            image.width = image.height = 2;
            image.format = format;
            image.premultiplied = true;
            Pixels px = readImage(image);
            ASSERT_EQ(px.rgba.size(), 16u);
            for (int ch = 0; ch < 4; ch++) {
                EXPECT_NEAR(px.rgba[ch], c.expected[ch], format == ColorFormat::RGBA ? 1.5f * q : 2e-3f)
                    << (c.mode == BlendMode::MAX_PREMULTIPLIED ? "max" : "light") << " channel " << ch << " format " << (int)format;
            }
        }
    }
}
