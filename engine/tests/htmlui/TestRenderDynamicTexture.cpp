// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RGBA8/R8 dynamic textures (phase 1 wave 0): TextureRender::createDynamicTexture with a
// format and filter, HtmlUITexture::createDynamic/update (one update per frame), GPU readback
// of what was uploaded, the size check, invalid arguments, and no sokol object left behind.

#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"

#include "Engine.h"
#include "HtmlUITexture.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class DynamicTextureTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            TestSystem::get().clearLogs();
        }

        size_t errors(const std::string& text = ""){
            return TestSystem::get().countLogs(D_LOG_ERROR, text);
        }
    };

    std::vector<uint8_t> pattern(int width, int height, int bytesPerPixel, int seed){
        std::vector<uint8_t> data((size_t)width * height * bytesPerPixel);
        for (size_t i = 0; i < data.size(); i++){
            data[i] = (uint8_t)((i * 37 + (size_t)seed * 101 + 11) & 0xFF);
        }
        return data;
    }

    // a stream update is recorded into the frame's commands (Vulkan): it lands with the
    // frame's commit
    void commitFrame(){
        GpuEnvironment::drawFrames(1);
    }

    struct SokolIds {
        sg_image image;
        sg_sampler sampler;
        sg_view view;
    };

    SokolIds idsOf(TextureRender& texture){
        return {texture.backend.get(), texture.backend.getSampler(), texture.backend.getView()};
    }

    bool allReleased(const SokolIds& ids){
        return sg_query_image_state(ids.image) == SG_RESOURCESTATE_INVALID &&
               sg_query_sampler_state(ids.sampler) == SG_RESOURCESTATE_INVALID &&
               sg_query_view_state(ids.view) == SG_RESOURCESTATE_INVALID;
    }

}

TEST_F(DynamicTextureTest, Rgba8UpdateAndReadback){
    // odd width: rows are tightly packed whatever the alignment
    const int width = 5, height = 3;
    std::unique_ptr<HtmlUITexture> texture = HtmlUITexture::createDynamic("test-dynamic-rgba", width, height);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->kind, HtmlUITexture::Kind::DYNAMIC);
    EXPECT_EQ(texture->render, texture->ownedRender.get());
    EXPECT_EQ(texture->width, width);
    EXPECT_EQ(texture->height, height);
    EXPECT_TRUE(texture->render->isCreated());
    EXPECT_EQ(sg_query_image_desc(texture->render->backend.get()).pixel_format, SG_PIXELFORMAT_RGBA8);
    EXPECT_TRUE(sg_query_image_desc(texture->render->backend.get()).usage.stream_update);

    const std::vector<uint8_t> first = pattern(width, height, 4, 1);
    ASSERT_TRUE(texture->canUpdate());
    ASSERT_TRUE(texture->update(first.data(), first.size()));
    EXPECT_EQ(texture->lastUpdateFrame, SystemRender::getFrameCount());
    commitFrame();

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(*texture->render, width, height, 4, pixels));
    EXPECT_EQ(pixels, first);

    // in place, the next frame: same sokol image
    const sg_image image = texture->render->backend.get();
    const std::vector<uint8_t> second = pattern(width, height, 4, 2);
    ASSERT_TRUE(texture->update(second.data(), second.size()));
    commitFrame();
    EXPECT_EQ(texture->render->backend.get().id, image.id);
    ASSERT_TRUE(readTexturePixels(*texture->render, width, height, 4, pixels));
    EXPECT_EQ(pixels, second);

    EXPECT_EQ(errors(), 0u);
}

TEST_F(DynamicTextureTest, R8UpdateAndReadback){
    const int width = 7, height = 3;
    std::unique_ptr<HtmlUITexture> texture = HtmlUITexture::createDynamic("test-dynamic-r8", width, height,
                                                                         ColorFormat::RED, TextureFilter::NEAREST);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(sg_query_image_desc(texture->render->backend.get()).pixel_format, SG_PIXELFORMAT_R8);
    const sg_sampler_desc sampler = sg_query_sampler_desc(texture->render->backend.getSampler());
    EXPECT_EQ(sampler.min_filter, SG_FILTER_NEAREST);
    EXPECT_EQ(sampler.mag_filter, SG_FILTER_NEAREST);
    EXPECT_EQ(sampler.wrap_u, SG_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ(sampler.wrap_v, SG_WRAP_CLAMP_TO_EDGE);

    const std::vector<uint8_t> coverage = pattern(width, height, 1, 3);
    ASSERT_TRUE(texture->update(coverage.data(), coverage.size()));
    commitFrame();

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(*texture->render, width, height, 1, pixels));
    EXPECT_EQ(pixels, coverage);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(DynamicTextureTest, LinearFilterIsDefault){
    std::unique_ptr<HtmlUITexture> texture = HtmlUITexture::createDynamic("test-dynamic-linear", 4, 4);
    ASSERT_NE(texture, nullptr);
    const sg_sampler_desc sampler = sg_query_sampler_desc(texture->render->backend.getSampler());
    EXPECT_EQ(sampler.min_filter, SG_FILTER_LINEAR);
    EXPECT_EQ(sampler.mag_filter, SG_FILTER_LINEAR);
    // a mipmap filter samples level 0 with its base filter: there are no mipmaps
    TextureRender mipmapped;
    ASSERT_TRUE(mipmapped.createDynamicTexture("test-dynamic-mip", 4, 4, ColorFormat::RGBA, TextureFilter::NEAREST_MIPMAP_LINEAR));
    EXPECT_EQ(sg_query_sampler_desc(mipmapped.backend.getSampler()).min_filter, SG_FILTER_NEAREST);
    EXPECT_EQ(sg_query_image_desc(mipmapped.backend.get()).num_mipmaps, 1);
    mipmapped.destroyTexture();
}

TEST_F(DynamicTextureTest, OneUpdatePerFrame){
    const int width = 4, height = 2;
    std::unique_ptr<HtmlUITexture> texture = HtmlUITexture::createDynamic("test-dynamic-once", width, height);
    ASSERT_NE(texture, nullptr);

    const std::vector<uint8_t> first = pattern(width, height, 4, 4);
    const std::vector<uint8_t> second = pattern(width, height, 4, 5);
    ASSERT_TRUE(texture->update(first.data(), first.size()));

    // sokol takes one update per image and frame: the second is refused, nothing uploaded
    EXPECT_FALSE(texture->canUpdate());
    EXPECT_FALSE(texture->update(second.data(), second.size()));
    commitFrame();

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(*texture->render, width, height, 4, pixels));
    EXPECT_EQ(pixels, first);

    // the next frame takes it
    EXPECT_TRUE(texture->canUpdate());
    EXPECT_TRUE(texture->update(second.data(), second.size()));
    commitFrame();
    ASSERT_TRUE(readTexturePixels(*texture->render, width, height, 4, pixels));
    EXPECT_EQ(pixels, second);

    // nothing to upload, a dead texture, another kind: refused
    EXPECT_FALSE(texture->update(nullptr, second.size()));
    EXPECT_FALSE(texture->update(second.data(), 0));
    HtmlUITexture staticTexture;
    EXPECT_FALSE(staticTexture.canUpdate());
    EXPECT_FALSE(staticTexture.update(second.data(), second.size()));
}

TEST_F(DynamicTextureTest, WrongSizeUpdateIsRejected){
    const int width = 4, height = 4;
    TextureRender texture;
    ASSERT_TRUE(texture.createDynamicTexture("test-dynamic-size", width, height, ColorFormat::RGBA, TextureFilter::LINEAR));

    const std::vector<uint8_t> data = pattern(width, height, 4, 6);
    texture.updateTexture(data.data(), data.size());
    commitFrame();

    // sokol would read width * height * 4 bytes from the smaller buffer
    const std::vector<uint8_t> small(16, 0xFF);
    texture.updateTexture(small.data(), small.size());
    EXPECT_EQ(errors("Dynamic texture update of 16 bytes ignored: the texture takes 64"), 1u);
    commitFrame();

    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(texture, width, height, 4, pixels));
    EXPECT_EQ(pixels, data);
    texture.destroyTexture();
}

TEST_F(DynamicTextureTest, InvalidArgumentsFail){
    EXPECT_EQ(HtmlUITexture::createDynamic("test-dynamic-r16", 4, 4, ColorFormat::RED16), nullptr);
    EXPECT_EQ(errors("only RGBA8 and R8 are supported"), 1u);

    EXPECT_EQ(HtmlUITexture::createDynamic("test-dynamic-empty", 0, 4), nullptr);
    EXPECT_EQ(HtmlUITexture::createDynamic("test-dynamic-negative", 4, -1), nullptr);
    EXPECT_EQ(errors("invalid size"), 2u);
    EXPECT_EQ(errors("[HTML UI] could not create the"), 3u);

    // a created texture is not created again
    TextureRender texture;
    ASSERT_TRUE(texture.createDynamicTexture("test-dynamic-twice", 2, 2, ColorFormat::RGBA, TextureFilter::LINEAR));
    const sg_image image = texture.backend.get();
    EXPECT_FALSE(texture.createDynamicTexture("test-dynamic-twice", 2, 2, ColorFormat::RGBA, TextureFilter::LINEAR));
    EXPECT_EQ(texture.backend.get().id, image.id);
    texture.destroyTexture();
}

TEST_F(DynamicTextureTest, DestroyReleasesEverySokolObject){
    std::vector<SokolIds> released;
    for (int i = 0; i < 100; i++){
        std::unique_ptr<HtmlUITexture> texture = HtmlUITexture::createDynamic("test-dynamic-leak", 16, 8,
                                                                             (i % 2) ? ColorFormat::RED : ColorFormat::RGBA);
        ASSERT_NE(texture, nullptr);
        const SokolIds ids = idsOf(*texture->render);
        EXPECT_EQ(sg_query_image_state(ids.image), SG_RESOURCESTATE_VALID);
        EXPECT_EQ(sg_query_sampler_state(ids.sampler), SG_RESOURCESTATE_VALID);
        EXPECT_EQ(sg_query_view_state(ids.view), SG_RESOURCESTATE_VALID);
        if (i % 10 == 0){
            const std::vector<uint8_t> data = pattern(16, 8, (i % 2) ? 1 : 4, i);
            EXPECT_TRUE(texture->update(data.data(), data.size()));
            commitFrame();
        }
        released.push_back(ids);
        // deleting the struct destroys what it owns
    }
    for (const SokolIds& ids : released){
        EXPECT_TRUE(allReleased(ids));
    }

    // a dead texture (its GPU objects went with the view) is freed without GPU calls
    std::unique_ptr<HtmlUITexture> dead = HtmlUITexture::createDynamic("test-dynamic-dead", 4, 4);
    ASSERT_NE(dead, nullptr);
    const SokolIds deadIds = idsOf(*dead->render);
    dead->render->destroyTexture();   // what view loss does first
    dead->dead = true;
    EXPECT_FALSE(dead->canUpdate());
    dead.reset();
    EXPECT_TRUE(allReleased(deadIds));

    EXPECT_EQ(errors(), 0u);
}
