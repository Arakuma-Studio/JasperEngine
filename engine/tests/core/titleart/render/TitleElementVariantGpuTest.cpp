// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ELEMENT program's variants (render/TitleElementVariant.h, S80 Deviation 58): the whole
// program, every Material member behind uniforms, compiled for 4.3-5.3 s in llvmpipe's first draw
// (S86 Deviation 23: the editor hung on every new title). An element now draws with the variant of
// what its material shades.
// - TitleElementVariant.BitsFollowTheBlock: the bits are what element.glsl branches on.
// - TitleElementVariantGpu.CompileCostIsBounded: a title with every heavy member on one face (a
//   relief pattern, a relief texture, glass, reflection, satin, per-character spans) and its rims
//   plans its variants; the first draw of the largest is at most 40 % of the whole program's (the
//   driver's compile, timed in one process, so the ratio holds under load; measured 25 % on
//   llvmpipe) or under 250 ms where the driver compiles fast (a GPU). Red control in the test: the
//   same title planned with TITLE_ART_TEST_ELEMENT_WHOLE (every element on the whole program, as
//   before) is over the bound on llvmpipe.

#include "TitleRenderTestSupport.h"

#include "pool/ShaderPool.h"
#include "titleart/render/TitleElementVariant.h"
#include "titleart/support/TitleStandardTitle.h"
#include "titleart/support/TitleTestAssets.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr uint32_t ELEMENT = (uint32_t)TitleRenderProgram::Element;
    // the largest variant's first draw against the whole program's, and the floor under which a
    // first draw holds no frame (a GPU's compile)
    constexpr double MAX_SHARE_OF_WHOLE = 0.4;
    constexpr double FAST_DRIVER_MS = 250.0;

    // builds program `number` (frames pumped while the background compiler works); false with
    // `error` when it cannot be built or takes over two minutes
    bool build(TitleGpu& gpu, uint32_t number, std::string& error) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::minutes(2)) {
            if (gpu.ensureProgram(number, &error)) return true;
            if (!error.empty()) return false;
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            SystemRender::commit();
        }
        error = "not built within two minutes";
        return false;
    }

    // the first draw of `number` into `target`, ms: the driver's compile of the variant
    // (llvmpipe's LLVM JIT) is in it. Blended straight-alpha, a state no render draws an element
    // in, so a variant an earlier test drew compiles again here
    double firstDrawMs(TitleGpu& gpu, uint32_t number, TitleTarget* target, std::string& error) {
        if (!build(gpu, number, error)) return -1.0;
        TitleGpuPass pass;
        pass.program = (TitleRenderProgram)(number & 0x7Fu);
        pass.variant = number & ~0x7Fu;
        pass.target = target;
        pass.blend = BlendMode::ALPHA;
        TitleFirstDraws::SynchronousScope synchronous;
        const auto start = std::chrono::steady_clock::now();
        if (!gpu.run(pass, error)) return -1.0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        SystemRender::commit();
        return ms;
    }

    // the standard title with every heavy Material member on its face (S83's patterns and the
    // textures with relief included), its rims as they are
    TitleArtDoc heavyTitle(TestAssetResolver& assets) {
        TitleImageData sky;
        sky.width = 8;
        sky.height = 4;
        sky.rgba.assign((size_t)sky.width * sky.height * 4, 200);
        assets.addImage("sky.png", sky);
        Json layer = standardTitleLayer();
        Json& face = layer["style"]["face"];
        face["fill"]["span"] = "char";
        face["pattern"] = Json{{"kind", "giraffe"}, {"scale", 20}, {"relief", 0.6}};
        face["texture"] = Json{{"kind", "liquid"}, {"amount", 0.5}};
        face["glass"] = Json{{"enabled", true}};
        face["reflection"] = Json{{"enabled", true}, {"map", {{"path", "sky.png"}, {"guid", ""}}}};
        face["satin"] = Json{{"enabled", true}};
        return standardDocument({layer});
    }

    // the ELEMENT programs a render of `doc` plans
    std::vector<uint32_t> plannedElements(const TitleArtDoc& doc, const TestAssetResolver& assets, uint32_t flags) {
        TitleArtRenderer renderer;
        renderer.setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = std::make_shared<const TitleArtDoc>(doc);
        r.pxScale = 0.5f;
        r.assets = &assets;
        r.testFlags = flags;
        std::unique_ptr<TitleRenderJob> job = renderer.begin(r);
        std::vector<uint32_t> out;
        if (!job) return out;
        for (const ShaderKey& k : job->shaderKeys()) {
            const uint32_t properties = (uint32_t)ShaderPool::getPropertiesFromKey(k);
            if ((properties & 0x7Fu) == ELEMENT) out.push_back(properties);
        }
        return out;
    }

    class TitleElementVariantGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

}

TEST(TitleElementVariant, BitsFollowTheBlock) {
    TitleParams p;
    EXPECT_EQ(titleElementVariant(p), 0u) << "a plain element (and the knockout)";
    p.set(30, 2.0f);                       // tiger, no relief
    EXPECT_EQ(titleElementVariant(p), 2u << TITLE_ELEMENT_PATTERN_SHIFT);
    p.set(32, 0.0f, 0.0f, 0.5f);           // with relief
    EXPECT_EQ(titleElementVariant(p), (2u << TITLE_ELEMENT_PATTERN_SHIFT) | TITLE_ELEMENT_RELIEF);
    p.set(30, 0.0f);                       // relief without a pattern reads nothing
    EXPECT_EQ(titleElementVariant(p), 0u);
    p.set(20, 0.0f, 0.0f, 0.0f, 9.0f);     // liquid with no amount and no sheen: not read
    EXPECT_EQ(titleElementVariant(p), 0u);
    p.set(21, 0.3f);
    EXPECT_EQ(titleElementVariant(p), 9u << TITLE_ELEMENT_TEXTURE_SHIFT);
    p.set(21, 0.0f);
    p.set(23, 1.0f);                       // the sheen reads the grain
    EXPECT_EQ(titleElementVariant(p), 9u << TITLE_ELEMENT_TEXTURE_SHIFT);
    TitleParams q;
    q.set(13, 0.0f, 0.0f, 0.0f, 1.0f);
    q.set(16, 1.0f);
    q.set(18, 1.0f);
    q.set(8, 0.0f, 0.0f, 0.0f, 2.0f);
    EXPECT_EQ(titleElementVariant(q), TITLE_ELEMENT_GLASS | TITLE_ELEMENT_REFLECTION | TITLE_ELEMENT_SATIN | TITLE_ELEMENT_SPAN);
}

TEST_F(TitleElementVariantGpu, CompileCostIsBounded) {
    TestAssetResolver assets;
    const TitleArtDoc doc = heavyTitle(assets);
    const std::vector<uint32_t> variants = plannedElements(doc, assets, 0);
    const std::vector<uint32_t> before = plannedElements(doc, assets, TITLE_ART_TEST_ELEMENT_WHOLE);
    ASSERT_GE(variants.size(), 2u) << "the face and its rims draw with variants of their own";
    ASSERT_EQ(before, std::vector<uint32_t>{ELEMENT | TITLE_ELEMENT_WHOLE});
    const uint32_t heavy = *std::max_element(variants.begin(), variants.end(), [](uint32_t a, uint32_t b) {
        return std::bitset<32>(a & TITLE_ELEMENT_VARIANT_BITS).count() < std::bitset<32>(b & TITLE_ELEMENT_VARIANT_BITS).count();
    });
    EXPECT_TRUE(heavy & TITLE_ELEMENT_RELIEF) << "the face's variant has its relief: " << ShaderPool::getShaderStr(ShaderType::TITLE_ART, heavy);

    TargetKey key;
    key.width = 64;
    key.height = 64;
    key.format = SystemRender::getFeatures().renderRGBA16F ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    std::string error;
    SokolTitleTargetAllocator allocator;
    std::unique_ptr<TitleTarget> target = allocator.create(key, &error);
    ASSERT_TRUE(target) << error;
    TitleGpu gpu;
    double largest = 0.0;
    std::string largestName;
    for (uint32_t v : variants) {
        const double ms = firstDrawMs(gpu, v, target.get(), error);
        ASSERT_GE(ms, 0.0) << ShaderPool::getShaderStr(ShaderType::TITLE_ART, v) << ": " << error;
        std::printf("[ element ] %-56s %8.1f ms\n", ShaderPool::getShaderStr(ShaderType::TITLE_ART, v).c_str(), ms);
        if (ms > largest) {
            largest = ms;
            largestName = ShaderPool::getShaderStr(ShaderType::TITLE_ART, v);
        }
    }
    const double whole = firstDrawMs(gpu, before[0], target.get(), error);
    ASSERT_GE(whole, 0.0) << error;
    std::printf("[ element ] %-56s %8.1f ms (before: every element)\n", ShaderPool::getShaderStr(ShaderType::TITLE_ART, before[0]).c_str(), whole);
    const double bound = std::max(MAX_SHARE_OF_WHOLE * whole, FAST_DRIVER_MS);
    ::testing::Test::RecordProperty("largest", largestName + " " + std::to_string(largest) + " ms of the whole program's " + std::to_string(whole) + " ms");
    EXPECT_LE(largest, bound) << largestName << " " << largest << " ms, the whole program " << whole << " ms";
    // red control: planned as before, every element draws with the whole program, over the bound
    const std::string device = SystemRender::getDeviceName();
    if (device.find("llvmpipe") != std::string::npos) { EXPECT_GT(whole, bound) << "the bound tells the whole program from its variants"; }
}
