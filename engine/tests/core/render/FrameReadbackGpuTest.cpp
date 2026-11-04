// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S80 R36, AC28: FrameReadback::readTexture reads float targets (8 bytes per pixel: RGBA16F as
// IEEE halves; 16: RGBA32F) exactly, rows top first, on the backend the suite runs on (GL core
// and Vulkan in CI; D3D11 and Metal by the same test on their runners).

#include "GpuEnvironment.h"

#include "render/FrameReadback.h"
#include "render/SystemRender.h"
#include "texture/Framebuffer.h"
#include "texture/TexturePrepare.h"
#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class FrameReadbackGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
        }
    };

    // a target cleared to `value` and read back with `bytesPerPixel`; the values of each channel
    bool clearAndRead(ColorFormat format, const float value[4], int bytesPerPixel, std::vector<float>& out, std::string& error) {
        const int width = 5, height = 3;
        Framebuffer target;
        FramebufferDesc desc;
        desc.width = width;
        desc.height = height;
        desc.color = format;
        desc.depth = DepthStencilFormat::NONE;
        desc.minFilter = desc.magFilter = TextureFilter::NEAREST;
        desc.label = "frame-readback-float-test";
        if (!target.create(desc)) {
            error = "the target was not created";
            return false;
        }
        SystemRender::commit();
        sg_pass pass = {};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {value[0], value[1], value[2], value[3]};
        pass.attachments = target.getRender().backend.get(0);
        sg_begin_pass(&pass);
        sg_end_pass();
        SystemRender::commit();

        std::vector<uint8_t> bytes;
        const bool ok = FrameReadback::readTexture(target.getRender().getColorTexture(), width, height, bytesPerPixel, bytes, &error);
        target.destroy();
        if (!ok) return false;
        if (bytes.size() != static_cast<size_t>(width) * height * bytesPerPixel) {
            error = "wrong byte count " + std::to_string(bytes.size());
            return false;
        }
        out.clear();
        for (size_t i = 0; i < bytes.size() / (bytesPerPixel / 4); i++) {
            if (bytesPerPixel == 16) {
                float v;
                std::memcpy(&v, bytes.data() + i * 4, 4);
                out.push_back(v);
            } else {
                uint16_t h;
                std::memcpy(&h, bytes.data() + i * 2, 2);
                out.push_back(halfToFloat(h));
            }
        }
        return true;
    }

}

TEST_F(FrameReadbackGpu, FloatTargetsRoundTrip) {
    const RenderFeatures& features = SystemRender::getFeatures();
    // values a half holds exactly, and two it rounds (1/3 and -1 / 3 to the nearest half)
    const float first[4] = {0.0f, 0.5f, 1.0f, 4.0f};
    const float second[4] = {1.0f / 3.0f, -1.0f, 0.25f, 2.0f};
    int checked = 0;
    for (const float* value : {first, second}) {
        if (features.renderRGBA32F) {
            std::vector<float> got;
            std::string error;
            ASSERT_TRUE(clearAndRead(ColorFormat::RGBA32F, value, 16, got, error)) << error;
            for (size_t i = 0; i < got.size(); i++) EXPECT_EQ(got[i], value[i % 4]) << "RGBA32F channel " << i % 4 << " of texel " << i / 4;
            checked++;
        }
        if (features.renderRGBA16F) {
            std::vector<float> got;
            std::string error;
            ASSERT_TRUE(clearAndRead(ColorFormat::RGBA16F, value, 8, got, error)) << error;
            for (size_t i = 0; i < got.size(); i++) {
                EXPECT_EQ(got[i], halfToFloat(floatToHalf(value[i % 4]))) << "RGBA16F channel " << i % 4 << " of texel " << i / 4;
            }
            checked++;
        }
    }
    if (checked == 0) GTEST_SKIP() << "the device renders no float target";
}

TEST_F(FrameReadbackGpu, RefusesOtherSizes) {
    Framebuffer target;
    FramebufferDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.depth = DepthStencilFormat::NONE;
    desc.label = "frame-readback-size-test";
    ASSERT_TRUE(target.create(desc));
    SystemRender::commit();
    std::vector<uint8_t> bytes;
    std::string error;
    EXPECT_FALSE(FrameReadback::readTexture(target.getRender().getColorTexture(), 2, 2, 12, bytes, &error));
    EXPECT_EQ(error, "bad readback size");
    target.destroy();
}
