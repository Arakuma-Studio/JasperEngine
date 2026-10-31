// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Immutable float textures made from data (vertex animation spec 3.2.1: the VAT payload is an
// RGBA16F/RGBA32F texture read with texelFetch). SokolTexture::createTexture maps RGBA16F,
// RGBA32F, R32F, RG16F and R16F to their sokol formats with no mipmaps, checks the data size and
// the device limit, honours nearest + clamp, and falls back to nearest where the device can't
// filter a format linearly. The GPU holds the exact values: read back raw, then drawn into an
// RGBA32F target by a texelFetch shader (GL backends: raw GLSL) and by the precompiled HTML UI
// passthrough program at texel centres (every backend), and read back bit for bit. Texture::setData
// uploads float TextureData through TexturePool. TextureData knows the bytes of each format and
// walks float texels whole (crop, resize, fitSize, flip) or refuses with an error. The RGBA8 and
// R8 paths are unchanged (generated mipmaps, the uploaded bytes).

#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"

#include "pool/ShaderPool.h"
#include "render/PipelineState.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "sokol/SokolPipelineCache.h"
#include "texture/Framebuffer.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 5;    // odd: rows are tightly packed whatever the alignment
    const int height = 3;

    size_t errors(const std::string& text = ""){
        return TestSystem::get().countLogs(D_LOG_ERROR, text);
    }

    float halfBitsToFloat(uint16_t h){
        const uint32_t sign = (h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1Fu;
        const uint32_t mantissa = h & 0x3FFu;
        if (exponent == 0){
            const float value = std::ldexp(static_cast<float>(mantissa), -24);
            return sign ? -value : value;
        }
        uint32_t bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        if (exponent == 31)
            bits = sign | 0x7F800000u | (mantissa << 13);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // RGBA16F texels: chosen halves (1, -2, the largest, the smallest subnormal, 1/3, -0) and a
    // spread of others, none NaN or infinite
    std::vector<uint16_t> halfTexels(int count){
        std::vector<uint16_t> texels(static_cast<size_t>(count) * 4);
        for (size_t i = 0; i < texels.size(); i++){
            uint16_t h = static_cast<uint16_t>((i * 7919u + 1234u) & 0xFFFFu);
            if (((h >> 10) & 0x1F) == 0x1F)
                h = static_cast<uint16_t>(h & ~0x4000u);  // exponent 31 (inf/NaN) to a finite one
            texels[i] = h;
        }
        const uint16_t chosen[] = {0x3C00, 0xC000, 0x7BFF, 0x0001, 0x3555, 0x8000};
        std::memcpy(texels.data(), chosen, sizeof(chosen));
        return texels;
    }

    // RGBA32F texels: negatives, values far past 1, fractions; no NaN, infinity or subnormal
    // (a shader may flush subnormals)
    std::vector<float> floatTexels(int count){
        std::vector<float> texels(static_cast<size_t>(count) * 4);
        for (size_t i = 0; i < texels.size(); i++)
            texels[i] = (static_cast<float>(i * 7919 % 4093) - 2046.5f) * 1.37109375f + static_cast<float>(i) * 1e-4f;
        texels[0] = 3.0e38f;
        texels[1] = -1.0e-30f;
        texels[2] = 0.1f;
        texels[3] = -0.0f;
        texels[4] = 16777217.0f;
        return texels;
    }

    std::vector<float> asFloats(const std::vector<uint8_t>& bytes){
        std::vector<float> values(bytes.size() / 4);
        std::memcpy(values.data(), bytes.data(), values.size() * 4);
        return values;
    }

    ::testing::AssertionResult sameBits(const std::vector<float>& expected, const std::vector<float>& actual){
        if (expected.size() != actual.size())
            return ::testing::AssertionFailure() << actual.size() << " values instead of " << expected.size();
        for (size_t i = 0; i < expected.size(); i++){
            if (std::memcmp(&expected[i], &actual[i], 4) != 0)
                return ::testing::AssertionFailure() << "value " << i << " (texel " << i / 4 << ", channel " << i % 4
                                                     << ") is " << actual[i] << " instead of " << expected[i];
        }
        return ::testing::AssertionSuccess();
    }

    bool createFromData(TextureRender& texture, const std::string& label, ColorFormat format, const void* data, size_t size,
                        TextureFilter filter = TextureFilter::NEAREST, TextureWrap wrap = TextureWrap::CLAMP_TO_EDGE,
                        int w = width, int h = height){
        void* faces[1] = {const_cast<void*>(data)};
        size_t sizes[1] = {size};
        return texture.createTexture(label, w, h, format, TextureType::TEXTURE_2D, 1, faces, sizes, filter, filter, wrap, wrap);
    }

    sg_image_desc imageDesc(TextureRender& texture){
        return sg_query_image_desc(texture.backend.get());
    }

    sg_sampler_desc samplerDesc(TextureRender& texture){
        return sg_query_sampler_desc(texture.backend.getSampler());
    }

    // A width x height RGBA32F target the source is drawn into, one fragment per texel, then
    // read back: the floats the shader saw. `texelFetch`: the raw GLSL texelFetch program (GL
    // backends only), else the HTML UI passthrough program sampling at texel centres
    struct DrawRun {
        bool ok = false;
        std::string error;
        std::vector<float> rgba;
    };

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    sg_shader makeTexelFetchShader(){
    #if defined(SOKOL_GLCORE)
        #define FLOAT_TEXTURE_TEST_GLSL_HEADER "#version 410\n"
    #else
        #define FLOAT_TEXTURE_TEST_GLSL_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
    #endif
        static const char* vs =
            FLOAT_TEXTURE_TEST_GLSL_HEADER
            "void main(){\n"
            "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char* fs =
            FLOAT_TEXTURE_TEST_GLSL_HEADER
            "uniform sampler2D payload;\n"
            "out vec4 fragColor;\n"
            "void main(){\n"
            "    fragColor = texelFetch(payload, ivec2(gl_FragCoord.xy), 0);\n"
            "}\n";
        #undef FLOAT_TEXTURE_TEST_GLSL_HEADER
        sg_shader_desc desc = {};
        desc.vertex_func.source = vs;
        desc.fragment_func.source = fs;
        desc.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
        desc.views[0].texture.image_type = SG_IMAGETYPE_2D;
        desc.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT;
        desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
        desc.samplers[0].sampler_type = SG_SAMPLERTYPE_NONFILTERING;
        desc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
        desc.texture_sampler_pairs[0].view_slot = 0;
        desc.texture_sampler_pairs[0].sampler_slot = 0;
        desc.texture_sampler_pairs[0].glsl_name = "payload";
        desc.label = "float-texture-texelfetch";
        return sg_make_shader(&desc);
    }
#endif

    DrawRun drawIntoFloatTarget(TextureRender& source, bool texelFetch){
        DrawRun run;
        if (!SystemRender::getFeatures().renderRGBA32F){
            run.error = "RGBA32F is not renderable on this device";
            return run;
        }

        std::shared_ptr<ShaderRender> passthrough;
        sg_shader shader = {SG_INVALID_ID};
        int textureView = 0, textureSampler = 0, vsBlock = -1, fsBlock = -1;
        if (texelFetch){
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
            shader = makeTexelFetchShader();
#endif
            if (sg_query_shader_state(shader) != SG_RESOURCESTATE_VALID){
                run.error = "the texelFetch program did not compile";
                return run;
            }
        }else{
            const bool ready = GpuEnvironment::runFrames([&](){
                passthrough = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH));
                return passthrough && passthrough->isCreated();
            });
            if (!ready){
                run.error = "the passthrough program did not compile";
                return run;
            }
            shader = passthrough->backend.get();
            ShaderData& data = passthrough->shaderData;
            vsBlock = data.getUniformBlockIndexByName("u_vs_htmluiPost");
            fsBlock = data.getUniformBlockIndexByName("u_fs_htmluiPost");
            const std::pair<int, int> slot = data.getTextureIndexByName("u_htmluiPostTex");
            textureView = slot.first;
            textureSampler = slot.second;
        }

        Framebuffer target;
        FramebufferDesc desc;
        desc.width = width;
        desc.height = height;
        desc.color = ColorFormat::RGBA32F;
        desc.depth = DepthStencilFormat::NONE;
        desc.minFilter = desc.magFilter = TextureFilter::NEAREST;
        desc.label = "float-texture-test-target";
        if (!target.create(desc)){
            run.error = "the RGBA32F target was not created";
            if (texelFetch)
                sg_destroy_shader(shader);
            return run;
        }
        SystemRender::commit();

        sg_pipeline pipeline;
        if (texelFetch){
            sg_pipeline_desc pipelineDesc = {};
            pipelineDesc.shader = shader;
            pipelineDesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
            pipelineDesc.color_count = 1;
            pipelineDesc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA32F;
            pipelineDesc.depth.pixel_format = SG_PIXELFORMAT_NONE;
            pipelineDesc.sample_count = 1;
            pipelineDesc.label = "float-texture-texelfetch-pipeline";
            pipeline = sg_make_pipeline(&pipelineDesc);
        }else{
            sg_pipeline_desc base = {};
            base.shader = shader;
            base.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
            base.label = "float-texture-passthrough-pipeline";
            PipelineState state;
            state.target.color = ColorFormat::RGBA32F;
            state.target.depth = DepthStencilFormat::NONE;
            state.blend = BlendMode::NONE;
            const sg_pipeline_desc pipelineDesc = SokolPipelineCache::makeDesc(base, state);
            pipeline = sg_make_pipeline(&pipelineDesc);
        }
        if (sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID){
            run.error = "pipeline not valid";
        }else{
            sg_pass pass = {};
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].store_action = SG_STOREACTION_STORE;
            pass.action.colors[0].clear_value = {-9.0f, -9.0f, -9.0f, -9.0f};
            pass.attachments = target.getRender().backend.get(0);
            sg_begin_pass(&pass);
            sg_apply_pipeline(pipeline);
            sg_bindings bindings = {};
            bindings.views[textureView] = source.backend.getView();
            bindings.samplers[textureSampler] = source.backend.getSampler();
            sg_apply_bindings(&bindings);
            if (!texelFetch){
                const float uvTransform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
                float fsParams[32] = {};
                fsParams[0] = 1.0f;
                sg_apply_uniforms(vsBlock, sg_range{uvTransform, sizeof(uvTransform)});
                sg_apply_uniforms(fsBlock, sg_range{fsParams, sizeof(fsParams)});
            }
            sg_draw(0, 3, 1);
            sg_end_pass();
            SystemRender::commit();

            std::vector<uint8_t> bytes;
            run.ok = readTextureRaw(target.getRender().getColorTexture(), width, height, ColorFormat::RGBA32F, bytes);
            if (run.ok)
                run.rgba = asFloats(bytes);
            else
                run.error = "readback failed";
        }
        sg_destroy_pipeline(pipeline);
        if (texelFetch)
            sg_destroy_shader(shader);
        target.destroy();
        return run;
    }

    class FloatTextureTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            TestSystem::get().clearLogs();
        }
    };

}

// ------------------------------------------------------------------------- formats (no GPU)

TEST(FloatTextureData, BytesAndChannelsOfEveryFormat){
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RGBA), 1);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RED), 1);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::SRGB8A8), 1);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RED16), 2);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RGBA16F), 2);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RG16F), 2);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::R16F), 2);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::RGBA32F), 4);
    EXPECT_EQ(TextureData::getBytesPerChannel(ColorFormat::R32F), 4);

    EXPECT_FALSE(TextureData::isFloatFormat(ColorFormat::RGBA));
    EXPECT_FALSE(TextureData::isFloatFormat(ColorFormat::RED16));
    EXPECT_TRUE(TextureData::isFloatFormat(ColorFormat::RGBA16F));
    EXPECT_TRUE(TextureData::isFloatFormat(ColorFormat::R32F));
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::RGBA16F), 4);
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::RGBA32F), 4);
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::RG16F), 2);
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::R16F), 1);
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::R32F), 1);
    EXPECT_EQ(TextureData::getFloatFormatChannels(ColorFormat::RGBA), 0);

    // the sokol format and bytes per texel of each
    const struct { ColorFormat format; sg_pixel_format pixel; size_t bytes; } expected[] = {
        {ColorFormat::RGBA, SG_PIXELFORMAT_RGBA8, 4},       {ColorFormat::RED, SG_PIXELFORMAT_R8, 1},
        {ColorFormat::RED16, SG_PIXELFORMAT_R16, 2},        {ColorFormat::RGBA16F, SG_PIXELFORMAT_RGBA16F, 8},
        {ColorFormat::RGBA32F, SG_PIXELFORMAT_RGBA32F, 16}, {ColorFormat::R32F, SG_PIXELFORMAT_R32F, 4},
        {ColorFormat::RG16F, SG_PIXELFORMAT_RG16F, 4},      {ColorFormat::R16F, SG_PIXELFORMAT_R16F, 2},
    };
    for (const auto& e : expected){
        sg_pixel_format pixel;
        size_t bytes = 0;
        ASSERT_TRUE(SokolTexture::getDataPixelFormat(e.format, pixel, bytes)) << (int)e.format;
        EXPECT_EQ(pixel, e.pixel) << (int)e.format;
        EXPECT_EQ(bytes, e.bytes) << (int)e.format;
        EXPECT_EQ(bytes, (size_t)(TextureData::getBytesPerChannel(e.format) *
                                  (TextureData::isFloatFormat(e.format) ? TextureData::getFloatFormatChannels(e.format) : (e.format == ColorFormat::RGBA ? 4 : 1))));
    }
    sg_pixel_format pixel;
    size_t bytes = 0;
    EXPECT_FALSE(SokolTexture::getDataPixelFormat(ColorFormat::SRGB8A8, pixel, bytes));
    EXPECT_EQ(bytes, 0u);
}

TEST(FloatTextureData, LinearFilterOnlyWhereTheDeviceFilters){
    RenderFeatures none;
    RenderFeatures all;
    all.filterRGBA16F = all.filterRGBA32F = all.filterR32F = all.filterRG16F = all.filterR16F = true;
    for (ColorFormat format : {ColorFormat::RGBA16F, ColorFormat::RGBA32F, ColorFormat::R32F, ColorFormat::RG16F, ColorFormat::R16F}){
        EXPECT_FALSE(SokolTexture::isLinearFilterable(format, none)) << (int)format;
        EXPECT_TRUE(SokolTexture::isLinearFilterable(format, all)) << (int)format;
    }
    // a GLES3 device without OES_texture_float_linear: the halves filter, the 32-bit ones don't
    RenderFeatures gles = none;
    gles.filterRGBA16F = gles.filterRG16F = gles.filterR16F = true;
    EXPECT_TRUE(SokolTexture::isLinearFilterable(ColorFormat::RGBA16F, gles));
    EXPECT_FALSE(SokolTexture::isLinearFilterable(ColorFormat::RGBA32F, gles));
    EXPECT_FALSE(SokolTexture::isLinearFilterable(ColorFormat::R32F, gles));
    // 8- and 16-bit unorm always
    EXPECT_TRUE(SokolTexture::isLinearFilterable(ColorFormat::RGBA, none));
    EXPECT_TRUE(SokolTexture::isLinearFilterable(ColorFormat::RED, none));
    EXPECT_TRUE(SokolTexture::isLinearFilterable(ColorFormat::RED16, none));
}

TEST(FloatTextureData, WalksFloatTexelsWhole){
    TestSystem::get().clearLogs();
    // 4 x 3 RGBA32F: texel (x, y) = (x, y, 10x + y, 0.5)
    const int w = 4, h = 3;
    // malloc'd (the walks free and replace it), owned by the TextureData made of it
    auto makeTexels = [&](){
        float* texels = static_cast<float*>(std::malloc(sizeof(float) * w * h * 4));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++){
                float* t = texels + (y * w + x) * 4;
                t[0] = (float)x; t[1] = (float)y; t[2] = 10.0f * x + y; t[3] = 0.5f;
            }
        return texels;
    };
    auto texel = [](TextureData& data, int x, int y){
        return static_cast<float*>(data.getData()) + (y * data.getWidth() + x) * 4;
    };

    {
        TextureData data(w, h, sizeof(float) * w * h * 4, ColorFormat::RGBA32F, 4, makeTexels());
        data.setDataOwned(true);
        EXPECT_TRUE(data.hasAlpha());
        data.crop(1, 1, 2, 2);
        ASSERT_EQ(data.getWidth(), 2);
        ASSERT_EQ(data.getSize(), sizeof(float) * 2 * 2 * 4);
        EXPECT_EQ(texel(data, 0, 0)[2], 11.0f);
        EXPECT_EQ(texel(data, 1, 1)[2], 22.0f);
        data.flipVertical();
        EXPECT_EQ(texel(data, 0, 0)[2], 12.0f);
        EXPECT_EQ(texel(data, 1, 1)[2], 21.0f);
        data.resize(4, 4);   // nearest: each texel doubled
        ASSERT_EQ(data.getSize(), sizeof(float) * 4 * 4 * 4);
        EXPECT_EQ(texel(data, 1, 0)[2], 12.0f);
        EXPECT_EQ(texel(data, 3, 3)[2], 21.0f);
        data.fitPowerOfTwo();  // 4 x 4 already
        data.fitSize(0, 0, 8, 4);
        ASSERT_EQ(data.getWidth(), 8);
        EXPECT_EQ(texel(data, 3, 3)[2], 21.0f);
        EXPECT_EQ(texel(data, 5, 2)[3], 0.0f);   // the new area is zero
        // 8-bit only: an error, 0
        EXPECT_EQ(data.getColorComponent(0, 0, 0), 0);
        EXPECT_EQ(errors("getColorComponent: colour format RGBA32F is not 8-bit"), 1u);
        TextureData copy(data);   // shallow, every field
        EXPECT_EQ(copy.getColorFormat(), ColorFormat::RGBA32F);
        EXPECT_EQ(copy.getSize(), data.getSize());
        EXPECT_EQ(copy.getData(), data.getData());
        copy.setDataOwned(false);
    }

    // alpha of the float formats: != 1
    {
        float opaque[8] = {0, 0, 0, 1, 5, 5, 5, 1};
        TextureData data(2, 1, sizeof(opaque), ColorFormat::RGBA32F, 4, opaque);
        EXPECT_FALSE(data.hasAlpha());
        opaque[7] = 0.999f;
        EXPECT_TRUE(data.hasAlpha());
        uint16_t halves[8] = {0, 0, 0, 0x3C00, 0x4000, 0, 0, 0x3C00};
        TextureData half(2, 1, sizeof(halves), ColorFormat::RGBA16F, 4, halves);
        EXPECT_FALSE(half.hasAlpha());
        halves[3] = 0x3800;
        EXPECT_TRUE(half.hasAlpha());
    }

    // a channel count that isn't the format's: every walk refuses, the data unchanged
    {
        TestSystem::get().clearLogs();
        float values[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        TextureData data(3, 1, sizeof(values), ColorFormat::RGBA32F, 1, values);
        data.crop(0, 0, 1, 1);
        data.resize(6, 2);
        data.fitSize(0, 0, 4, 4);
        data.flipVertical();
        EXPECT_EQ(data.getData(), (void*)values);
        EXPECT_EQ(data.getWidth(), 3);
        EXPECT_EQ(values[11], 12.0f);
        EXPECT_EQ(errors("colour format RGBA32F has 4 channels, the data says 1"), 4u);
    }

    // out-of-range crop/fit: refused instead of reading or writing outside the buffer
    {
        TestSystem::get().clearLogs();
        TextureData data(w, h, sizeof(float) * w * h * 4, ColorFormat::RGBA32F, 4, makeTexels());
        data.setDataOwned(true);
        data.crop(3, 0, 2, 2);
        data.fitSize(1, 0, 4, 4);
        EXPECT_EQ(data.getWidth(), 4);
        EXPECT_EQ(errors("is outside the 4 x 3 image"), 1u);
        EXPECT_EQ(errors("doesn't fit 4 x 4"), 1u);
    }
}

TEST(FloatTextureData, EightBitWalksUnchanged){
    // RGBA8 crop, flip and resize against a reference computed here
    const int w = 5, h = 4;
    unsigned char* pixels = static_cast<unsigned char*>(std::malloc(w * h * 4));
    for (int i = 0; i < w * h * 4; i++)
        pixels[i] = static_cast<unsigned char>(i * 13 + 1);
    std::vector<unsigned char> source(pixels, pixels + w * h * 4);
    TextureData data(w, h, w * h * 4, ColorFormat::RGBA, 4, pixels);
    data.setDataOwned(true);
    EXPECT_TRUE(data.hasAlpha());
    data.crop(1, 1, 3, 2);
    std::vector<unsigned char> expected;
    for (int y = 1; y < 3; y++)
        for (int x = 1; x < 4; x++)
            for (int c = 0; c < 4; c++)
                expected.push_back(source[(y * w + x) * 4 + c]);
    ASSERT_EQ(data.getSize(), expected.size());
    EXPECT_EQ(std::memcmp(data.getData(), expected.data(), expected.size()), 0);
    EXPECT_EQ(data.getColorComponent(2, 1, 3), expected[(1 * 3 + 2) * 4 + 3]);
    data.flipVertical();
    EXPECT_EQ(std::memcmp(data.getData(), expected.data() + 12, 12), 0);
    EXPECT_EQ(std::memcmp((unsigned char*)data.getData() + 12, expected.data(), 12), 0);

    // RED16 (two bytes a channel) flips whole texels
    uint16_t* heights = static_cast<uint16_t*>(std::malloc(2 * 2 * sizeof(uint16_t)));
    heights[0] = 0x0102; heights[1] = 0x0304; heights[2] = 0x0506; heights[3] = 0x0708;
    TextureData red16(2, 2, 8, ColorFormat::RED16, 1, heights);
    red16.setDataOwned(true);
    red16.flipVertical();
    const uint16_t* flipped = static_cast<const uint16_t*>(red16.getData());
    EXPECT_EQ(flipped[0], 0x0506);
    EXPECT_EQ(flipped[1], 0x0708);
    EXPECT_EQ(flipped[2], 0x0102);
}

// ------------------------------------------------------------------------------------- GPU

TEST_F(FloatTextureTest, Rgba16fAnd32fHoldExactValues){
    const std::vector<uint16_t> halves = halfTexels(width * height);
    const std::vector<float> floats = floatTexels(width * height);
    std::vector<float> halvesAsFloats(halves.size());
    for (size_t i = 0; i < halves.size(); i++)
        halvesAsFloats[i] = halfBitsToFloat(halves[i]);

    for (ColorFormat format : {ColorFormat::RGBA16F, ColorFormat::RGBA32F}){
        const bool half = format == ColorFormat::RGBA16F;
        SCOPED_TRACE(half ? "RGBA16F" : "RGBA32F");
        const void* data = half ? static_cast<const void*>(halves.data()) : static_cast<const void*>(floats.data());
        const size_t size = half ? halves.size() * 2 : floats.size() * 4;
        const std::vector<float>& expected = half ? halvesAsFloats : floats;

        TextureRender texture;
        ASSERT_TRUE(createFromData(texture, "float-texture-exact", format, data, size));
        EXPECT_EQ(errors(), 0u);
        const sg_image_desc image = imageDesc(texture);
        EXPECT_EQ(image.pixel_format, half ? SG_PIXELFORMAT_RGBA16F : SG_PIXELFORMAT_RGBA32F);
        EXPECT_EQ(image.num_mipmaps, 1);
        EXPECT_EQ(image.width, width);
        EXPECT_EQ(image.height, height);
        const sg_sampler_desc sampler = samplerDesc(texture);
        EXPECT_EQ(sampler.min_filter, SG_FILTER_NEAREST);
        EXPECT_EQ(sampler.mag_filter, SG_FILTER_NEAREST);
        EXPECT_EQ(sampler.wrap_u, SG_WRAP_CLAMP_TO_EDGE);
        EXPECT_EQ(sampler.wrap_v, SG_WRAP_CLAMP_TO_EDGE);

        // the image holds the uploaded bits
        std::vector<uint8_t> raw;
        ASSERT_TRUE(readTextureRaw(texture, width, height, format, raw));
        ASSERT_EQ(raw.size(), size);
        EXPECT_EQ(std::memcmp(raw.data(), data, size), 0);

        // a shader reads the exact values: texelFetch (GL), the passthrough at texel centres (all)
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
        {
            const DrawRun run = drawIntoFloatTarget(texture, true);
            ASSERT_TRUE(run.ok) << run.error;
            EXPECT_TRUE(sameBits(expected, run.rgba)) << "texelFetch";
        }
#endif
        {
            const DrawRun run = drawIntoFloatTarget(texture, false);
            ASSERT_TRUE(run.ok) << run.error;
            EXPECT_TRUE(sameBits(expected, run.rgba)) << "passthrough";
        }
        texture.destroyTexture();
    }
}

TEST_F(FloatTextureTest, EveryFloatFormatUploads){
    // R32F, RG16F, R16F too: format, no mipmaps, the bits
    const struct { ColorFormat format; sg_pixel_format pixel; size_t bytes; } formats[] = {
        {ColorFormat::R32F, SG_PIXELFORMAT_R32F, 4}, {ColorFormat::RG16F, SG_PIXELFORMAT_RG16F, 4},
        {ColorFormat::R16F, SG_PIXELFORMAT_R16F, 2},
    };
    const std::vector<uint16_t> halves = halfTexels(width * height);   // enough bytes for each
    const std::vector<float> floats = floatTexels(width * height);
    for (const auto& f : formats){
        SCOPED_TRACE((int)f.format);
        const void* data = f.format == ColorFormat::R32F ? static_cast<const void*>(floats.data()) : static_cast<const void*>(halves.data());
        const size_t size = static_cast<size_t>(width) * height * f.bytes;
        TextureRender texture;
        ASSERT_TRUE(createFromData(texture, "float-texture-format", f.format, data, size, TextureFilter::LINEAR_MIPMAP_LINEAR));
        EXPECT_EQ(imageDesc(texture).pixel_format, f.pixel);
        EXPECT_EQ(imageDesc(texture).num_mipmaps, 1);   // a mipmap filter generates nothing
        std::vector<uint8_t> raw;
        ASSERT_TRUE(readTextureRaw(texture, width, height, f.format, raw));
        ASSERT_EQ(raw.size(), size);
        EXPECT_EQ(std::memcmp(raw.data(), data, size), 0);
        texture.destroyTexture();
    }
    EXPECT_EQ(errors(), 0u);   // and no "mipmaps" error from the 8-bit generator
}

TEST_F(FloatTextureTest, LinearFilterKeptOrNearestFallback){
    const RenderFeatures& features = SystemRender::getFeatures();
    const std::vector<float> floats = floatTexels(width * height);
    const std::vector<uint16_t> halves = halfTexels(width * height);
    for (ColorFormat format : {ColorFormat::RGBA16F, ColorFormat::RGBA32F}){
        SCOPED_TRACE((int)format);
        TestSystem::get().clearLogs();
        const bool half = format == ColorFormat::RGBA16F;
        TextureRender texture;
        ASSERT_TRUE(createFromData(texture, "float-texture-linear", format,
                                   half ? static_cast<const void*>(halves.data()) : static_cast<const void*>(floats.data()),
                                   half ? halves.size() * 2 : floats.size() * 4, TextureFilter::LINEAR, TextureWrap::REPEAT));
        const sg_sampler_desc sampler = samplerDesc(texture);
        const bool filterable = SokolTexture::isLinearFilterable(format, features);
        EXPECT_EQ(filterable, half ? features.filterRGBA16F : features.filterRGBA32F);
        EXPECT_EQ(sampler.min_filter, filterable ? SG_FILTER_LINEAR : SG_FILTER_NEAREST);
        EXPECT_EQ(sampler.mag_filter, filterable ? SG_FILTER_LINEAR : SG_FILTER_NEAREST);
        EXPECT_EQ(sampler.mipmap_filter, SG_FILTER_NEAREST);   // one level
        EXPECT_EQ(sampler.wrap_u, SG_WRAP_REPEAT);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "can't filter colour format"), filterable ? 0u : 1u);
        texture.destroyTexture();
    }
}

TEST_F(FloatTextureTest, RefusesWhatCantBeUploaded){
    const std::vector<float> floats = floatTexels(width * height);
    TextureRender texture;
    // the data of another size
    EXPECT_FALSE(createFromData(texture, "float-texture-short", ColorFormat::RGBA32F, floats.data(), floats.size() * 4 - 4));
    EXPECT_EQ(errors("face 0 has 236 bytes of data, 5 x 3 RGBA32F takes 240"), 1u);
    EXPECT_FALSE(texture.isCreated());
    EXPECT_FALSE(createFromData(texture, "float-texture-half-of-float", ColorFormat::RGBA16F, floats.data(), floats.size() * 4));
    EXPECT_EQ(errors("5 x 3 RGBA16F takes 120"), 1u);
    // a format a texture can't be made from (an uninitialised format before)
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 255);
    EXPECT_FALSE(createFromData(texture, "float-texture-srgb", ColorFormat::SRGB8A8, rgba.data(), rgba.size()));
    EXPECT_EQ(errors("can't have colour format SRGB8A8"), 1u);
    // over the device's size limit
    const int limit = SystemRender::getFeatures().maxTextureSize2D;
    ASSERT_GT(limit, 0);
    std::vector<float> row(static_cast<size_t>(limit + 1) * 4, 1.0f);
    EXPECT_FALSE(createFromData(texture, "float-texture-wide", ColorFormat::RGBA32F, row.data(), row.size() * 4,
                                TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, limit + 1, 1));
    EXPECT_EQ(errors("the device's limit is"), 1u);
    EXPECT_FALSE(texture.isCreated());
}

TEST_F(FloatTextureTest, TextureSetDataThroughThePool){
    const std::vector<float> floats = floatTexels(width * height);
    const std::vector<uint16_t> halves = halfTexels(width * height);
    for (ColorFormat format : {ColorFormat::RGBA32F, ColorFormat::RGBA16F}){
        const bool half = format == ColorFormat::RGBA16F;
        SCOPED_TRACE(half ? "RGBA16F" : "RGBA32F");
        const void* bytes = half ? static_cast<const void*>(halves.data()) : static_cast<const void*>(floats.data());
        const size_t size = half ? halves.size() * 2 : floats.size() * 4;
        Texture texture;
        texture.setMinFilter(TextureFilter::NEAREST);
        texture.setMagFilter(TextureFilter::NEAREST);
        texture.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        texture.setWrapV(TextureWrap::CLAMP_TO_EDGE);
        texture.setData(half ? "float-texture-pool-16" : "float-texture-pool-32",
                        TextureData(width, height, static_cast<unsigned int>(size), format, 4, const_cast<void*>(bytes)));
        TextureRender* render = texture.getRender();
        ASSERT_NE(render, nullptr);
        ASSERT_TRUE(render->isCreated());
        EXPECT_EQ(imageDesc(*render).pixel_format, half ? SG_PIXELFORMAT_RGBA16F : SG_PIXELFORMAT_RGBA32F);
        EXPECT_EQ(samplerDesc(*render).min_filter, SG_FILTER_NEAREST);
        EXPECT_EQ(samplerDesc(*render).wrap_v, SG_WRAP_CLAMP_TO_EDGE);
        EXPECT_EQ(texture.getWidth(), (unsigned)width);
        std::vector<uint8_t> raw;
        ASSERT_TRUE(readTextureRaw(*render, width, height, format, raw));
        ASSERT_EQ(raw.size(), size);
        EXPECT_EQ(std::memcmp(raw.data(), bytes, size), 0);
        // the data stays the caller's (setData does not release it)
        EXPECT_EQ(texture.getData().getData(), bytes);
        texture.destroy();
    }
    EXPECT_EQ(errors(), 0u);
}

TEST_F(FloatTextureTest, EightBitPathsUnchanged){
    // RGBA8 with a mipmap filter: level 0 the uploaded bytes. createTexture makes no mipmaps
    // (S14: they are made on the CPU, the pool's prepared levels through createTextureLevels;
    // upstream's createTexture generated them), the pool's path below has the chain
    const int w = 8, h = 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); i++)
        rgba[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    TextureRender mipmapped;
    ASSERT_TRUE(createFromData(mipmapped, "rgba8-mipmapped", ColorFormat::RGBA, rgba.data(), rgba.size(),
                               TextureFilter::LINEAR_MIPMAP_LINEAR, TextureWrap::REPEAT, w, h));
    EXPECT_EQ(imageDesc(mipmapped).pixel_format, SG_PIXELFORMAT_RGBA8);
    EXPECT_EQ(imageDesc(mipmapped).num_mipmaps, 1);
    EXPECT_EQ(samplerDesc(mipmapped).min_filter, SG_FILTER_LINEAR);
    EXPECT_EQ(samplerDesc(mipmapped).mipmap_filter, SG_FILTER_LINEAR);
    std::vector<uint8_t> raw;
    ASSERT_TRUE(readTextureRaw(mipmapped, w, h, ColorFormat::RGBA, raw));
    EXPECT_EQ(raw, rgba);
    ASSERT_TRUE(readTexturePixels(mipmapped, w, h, 4, raw));
    EXPECT_EQ(raw, rgba);
    mipmapped.destroyTexture();
    {
        Texture pooled;
        pooled.setMinFilter(TextureFilter::LINEAR_MIPMAP_LINEAR);
        pooled.setData("rgba8-pooled-mipmapped", TextureData(w, h, static_cast<unsigned int>(rgba.size()), ColorFormat::RGBA, 4, rgba.data()));
        TextureRender* render = pooled.getRender();
        ASSERT_NE(render, nullptr);
        EXPECT_EQ(imageDesc(*render).pixel_format, SG_PIXELFORMAT_RGBA8);
        EXPECT_EQ(imageDesc(*render).num_mipmaps, 4);   // 8x4, 4x2, 2x1, 1x1
        EXPECT_EQ(samplerDesc(*render).mipmap_filter, SG_FILTER_LINEAR);
        pooled.destroy();
    }

    // RGBA8 nearest and R8 linear: one level, the bytes
    TextureRender nearest;
    ASSERT_TRUE(createFromData(nearest, "rgba8-nearest", ColorFormat::RGBA, rgba.data(), rgba.size(),
                               TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, w, h));
    EXPECT_EQ(imageDesc(nearest).num_mipmaps, 1);
    ASSERT_TRUE(readTextureRaw(nearest, w, h, ColorFormat::RGBA, raw));
    EXPECT_EQ(raw, rgba);
    nearest.destroyTexture();

    const std::vector<uint8_t> red(rgba.begin(), rgba.begin() + w * h);
    TextureRender r8;
    ASSERT_TRUE(createFromData(r8, "r8-linear", ColorFormat::RED, red.data(), red.size(), TextureFilter::LINEAR, TextureWrap::REPEAT, w, h));
    EXPECT_EQ(imageDesc(r8).pixel_format, SG_PIXELFORMAT_R8);
    EXPECT_EQ(samplerDesc(r8).min_filter, SG_FILTER_LINEAR);
    ASSERT_TRUE(readTextureRaw(r8, w, h, ColorFormat::RED, raw));
    EXPECT_EQ(raw, red);
    r8.destroyTexture();
    EXPECT_EQ(errors(), 0u);
}
