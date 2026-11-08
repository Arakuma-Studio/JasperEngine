// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ELEMENT program's variants (render/TitleElementVariant.h, S80 Deviation 58).

#include "TitleRenderTestSupport.h"

#include "pool/ShaderPool.h"
#include "titleart/render/TitleElementVariant.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    constexpr uint32_t ELEMENT = (uint32_t)TitleRenderProgram::Element;

    uint32_t patternBits(uint32_t kind) { return kind << TITLE_ELEMENT_PATTERN_SHIFT; }
    uint32_t textureBits(uint32_t kind) { return kind << TITLE_ELEMENT_TEXTURE_SHIFT; }

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

    // the first draw of `number` into `target` as an element pass draws (premultiplied over), ms:
    // the driver's compile of the variant (llvmpipe's LLVM JIT) is in it
    double firstDrawMs(TitleGpu& gpu, uint32_t number, TitleTarget* target, std::string& error) {
        if (!build(gpu, number, error)) return -1.0;
        TitleGpuPass pass;
        pass.program = (TitleRenderProgram)(number & 0x7Fu);
        pass.variant = number & ~0x7Fu;
        pass.target = target;
        pass.blend = BlendMode::PREMULTIPLIED;
        TitleFirstDraws::SynchronousScope synchronous;
        const auto start = std::chrono::steady_clock::now();
        if (!gpu.run(pass, error)) return -1.0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        SystemRender::commit();
        return ms;
    }

    class TitleElementVariantGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

}

TEST_F(TitleElementVariantGpu, FirstDrawCost) {
    TargetKey key;
    key.width = 64;
    key.height = 64;
    key.format = SystemRender::getFeatures().renderRGBA16F ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    std::string error;
    SokolTitleTargetAllocator allocator;
    std::unique_ptr<TitleTarget> target = allocator.create(key, &error);
    ASSERT_TRUE(target) << error;
    TitleGpu gpu;
    struct Row {
        std::string name;
        uint32_t number;
    };
    std::vector<Row> rows = {{"COPY", (uint32_t)TitleRenderProgram::Copy}, {"BLUR", (uint32_t)TitleRenderProgram::Blur},
                             {"HEIGHT", (uint32_t)TitleRenderProgram::Height}, {"element plain", ELEMENT}};
    const uint32_t bools = TITLE_ELEMENT_GLASS | TITLE_ELEMENT_REFLECTION | TITLE_ELEMENT_SATIN | TITLE_ELEMENT_SPAN;
    rows.push_back({"glass", ELEMENT | TITLE_ELEMENT_GLASS});
    rows.push_back({"reflection", ELEMENT | TITLE_ELEMENT_REFLECTION});
    rows.push_back({"satin", ELEMENT | TITLE_ELEMENT_SATIN});
    rows.push_back({"span", ELEMENT | TITLE_ELEMENT_SPAN});
    rows.push_back({"four bools", ELEMENT | bools});
    for (uint32_t k = 1; k < TITLE_ELEMENT_PATTERN_KINDS; k++) {
        rows.push_back({"pattern " + std::to_string(k) + " relief", ELEMENT | patternBits(k) | TITLE_ELEMENT_RELIEF});
    }
    for (uint32_t t = 1; t < TITLE_ELEMENT_TEXTURE_KINDS; t++) rows.push_back({"texture " + std::to_string(t), ELEMENT | textureBits(t)});
    rows.push_back({"pattern 2 relief, texture 9, bools", ELEMENT | patternBits(2) | TITLE_ELEMENT_RELIEF | textureBits(9) | bools});
    rows.push_back({"pattern 3 relief, texture 12, bools", ELEMENT | patternBits(3) | TITLE_ELEMENT_RELIEF | textureBits(12) | bools});
    rows.push_back({"whole", ELEMENT | TITLE_ELEMENT_WHOLE});
    for (const Row& r : rows) {
        const double ms = firstDrawMs(gpu, r.number, target.get(), error);
        ASSERT_GE(ms, 0.0) << r.name << ": " << error;
        std::printf("[ element ] %-40s %s %9.1f ms\n", r.name.c_str(), ShaderPool::getShaderStr(ShaderType::TITLE_ART, r.number).c_str(), ms);
    }
}
