// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The half-float two- and one-channel colour formats ColorFormat::RG16F and R16F (the fluid
// simulation's velocity and its pressure, divergence and curl fields) in the render layer:
// RenderFeatures says they can be rendered to, blended into and filtered (glcore and lavapipe
// can); Framebuffer creates them through every path (FramebufferDesc, the single-attachment
// create(), MRT) as SG_PIXELFORMAT_RG16F / R16F images; SokolPipelineCache builds pipelines for
// them. Then a value check through raw sokol passes with the HTML UI passthrough program: a 2 x 1
// RGBA8 texture is drawn twice, additively, into a 2 x 1 half-float target (values above 1 kept,
// the channels the format lacks dropped), which is drawn linearly filtered into a 1 x 1 RGBA8
// target at a quarter opacity and read back: the mean of the two texels, with 0 for the missing
// colour channels and 1 for alpha.

#include "harness/GpuEnvironment.h"
#include "harness/Readback.h"

#include "pool/ShaderPool.h"
#include "render/FramebufferRender.h"
#include "render/PipelineState.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "sokol/SokolPipelineCache.h"
#include "texture/Framebuffer.h"
#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    sg_pixel_format sokolFormatOf(ColorFormat format){
        return format == ColorFormat::RG16F ? SG_PIXELFORMAT_RG16F : SG_PIXELFORMAT_R16F;
    }

    const char* nameOf(ColorFormat format){
        return format == ColorFormat::RG16F ? "RG16F" : "R16F";
    }

    sg_pixel_format pixelFormatOf(TextureRender& texture){
        return sg_query_image_desc(texture.backend.get()).pixel_format;
    }

    struct HalfRun {
        bool ok = false;
        std::string error;
        std::vector<uint8_t> rgba; // the 1 x 1 result
    };

    HalfRun renderThroughHalfTarget(ColorFormat format){
        HalfRun run;
        std::shared_ptr<ShaderRender> shader;
        const bool ready = GpuEnvironment::runFrames([&](){
            shader = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH));
            return shader && shader->isCreated();
        });
        if (!ready){
            run.error = "the passthrough program did not compile";
            return run;
        }

        // texel 0 (0, 128, 255, 255), texel 1 (255, 0, 255, 255): B and A are what RG16F / R16F drop
        TextureRender source;
        const uint8_t texels[8] = {0, 128, 255, 255, 255, 0, 255, 255};
        void* sourceData[1] = {const_cast<uint8_t*>(texels)};
        size_t sourceSize[1] = {sizeof(texels)};
        Framebuffer half;
        FramebufferDesc halfDesc;
        halfDesc.width = 2;
        halfDesc.height = 1;
        halfDesc.color = format;
        halfDesc.depth = DepthStencilFormat::NONE;
        halfDesc.minFilter = halfDesc.magFilter = TextureFilter::LINEAR;
        halfDesc.label = "half-float-test";
        FramebufferDesc outDesc;
        outDesc.width = outDesc.height = 1;
        outDesc.depth = DepthStencilFormat::NONE;
        outDesc.minFilter = outDesc.magFilter = TextureFilter::NEAREST;
        outDesc.label = "half-float-test-out";
        FramebufferRender out;
        if (!source.createTexture("half-float-source", 2, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, sourceData, sourceSize,
                                  TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE) ||
            !half.create(halfDesc) || !out.createFramebuffer(outDesc)){
            run.error = "a texture or framebuffer was not created";
            return run;
        }
        SystemRender::commit();

        ShaderData& data = shader->shaderData;
        const int vsBlock = data.getUniformBlockIndexByName("u_vs_htmluiPost");
        const int fsBlock = data.getUniformBlockIndexByName("u_fs_htmluiPost");
        const std::pair<int, int> textureSlot = data.getTextureIndexByName("u_htmluiPostTex");

        // the pipelines as the dynamic path builds them
        sg_pipeline_desc base = {};
        base.shader = shader->backend.get();
        base.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
        base.label = "half-float-test-pipeline";
        PipelineState intoHalf;
        intoHalf.target.color = format;
        intoHalf.target.depth = DepthStencilFormat::NONE;
        intoHalf.blend = BlendMode::ADDITIVE_PREMULTIPLIED;
        PipelineState intoOut;
        intoOut.target.color = ColorFormat::RGBA;
        intoOut.target.depth = DepthStencilFormat::NONE;
        intoOut.blend = BlendMode::NONE;
        const sg_pipeline_desc halfPipelineDesc = SokolPipelineCache::makeDesc(base, intoHalf);
        if (halfPipelineDesc.colors[0].pixel_format != sokolFormatOf(format)){
            run.error = "the pipeline cache gave the half-float target another pixel format";
            return run;
        }
        const sg_pipeline halfPipeline = sg_make_pipeline(&halfPipelineDesc);
        const sg_pipeline_desc outPipelineDesc = SokolPipelineCache::makeDesc(base, intoOut);
        const sg_pipeline outPipeline = sg_make_pipeline(&outPipelineDesc);
        if (sg_query_pipeline_state(halfPipeline) != SG_RESOURCESTATE_VALID || sg_query_pipeline_state(outPipeline) != SG_RESOURCESTATE_VALID){
            run.error = "pipeline not valid";
            return run;
        }
        const float uvTransform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        float fsParams[32] = {};

        // two additive copies of the source: texel 1's R is 2, texel 0's G 2 x 128/255
        sg_pass pass = {};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_STORE;
        pass.action.colors[0].clear_value = {0.0f, 0.0f, 0.0f, 0.0f};
        pass.attachments = half.getRender().backend.get(0);
        sg_begin_pass(&pass);
        sg_apply_pipeline(halfPipeline);
        sg_bindings bindings = {};
        bindings.views[textureSlot.first] = source.backend.getView();
        bindings.samplers[textureSlot.second] = source.backend.getSampler();
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(vsBlock, sg_range{uvTransform, sizeof(uvTransform)});
        fsParams[0] = 1.0f;
        sg_apply_uniforms(fsBlock, sg_range{fsParams, sizeof(fsParams)});
        sg_draw(0, 3, 1);
        sg_draw(0, 3, 1);
        sg_end_pass();

        // the half-float target sampled linearly between its texels, at a quarter
        pass.attachments = out.backend.get(0);
        sg_begin_pass(&pass);
        sg_apply_pipeline(outPipeline);
        bindings = {};
        bindings.views[textureSlot.first] = half.getRender().getColorTexture().backend.getView();
        bindings.samplers[textureSlot.second] = half.getRender().getColorTexture().backend.getSampler();
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(vsBlock, sg_range{uvTransform, sizeof(uvTransform)});
        fsParams[0] = 0.25f;
        sg_apply_uniforms(fsBlock, sg_range{fsParams, sizeof(fsParams)});
        sg_draw(0, 3, 1);
        sg_end_pass();
        SystemRender::commit();

        run.ok = readTexture(out.getColorTexture(), 1, 1, run.rgba);
        if (!run.ok)
            run.error = "readback failed";
        sg_destroy_pipeline(halfPipeline);
        sg_destroy_pipeline(outPipeline);
        source.destroyTexture();
        half.destroy();
        out.destroyFramebuffer();
        return run;
    }

}

// sokol's sg_query_pixelformat for the two formats, through SystemRender::getFeatures: desktop
// GL 3.3 (Mesa llvmpipe) and Vulkan (lavapipe) render, blend and filter both

TEST(HalfFloatTargets, FramebuffersOfEveryPath){
    JASPER_REQUIRE_GPU();
    for (ColorFormat format : {ColorFormat::RG16F, ColorFormat::R16F}){
        SCOPED_TRACE(nameOf(format));
        // FramebufferDesc, with and without MSAA
        for (int samples : {1, 4}){
            FramebufferDesc desc;
            desc.width = 16;
            desc.height = 8;
            desc.color = format;
            desc.depth = DepthStencilFormat::NONE;
            desc.sampleCount = samples;
            desc.label = "half-float-desc";
            Framebuffer framebuffer;
            if (samples > 1 && !sg_query_pixelformat(sokolFormatOf(format)).msaa)
                continue;
            ASSERT_TRUE(framebuffer.create(desc)) << samples << " samples";
            EXPECT_TRUE(framebuffer.isCreated());
            EXPECT_EQ(pixelFormatOf(framebuffer.getRender().getColorTexture()), sokolFormatOf(format)) << samples << " samples";
            EXPECT_EQ(framebuffer.getRender().getSampleCount(), samples);
            framebuffer.destroy();
        }
        // the single-attachment create() (FramebufferDepth::NONE and a colour format)
        Framebuffer single;
        single.setWidth(8);
        single.setHeight(8);
        const ColorFormat formats[2] = {format, ColorFormat::RGBA};
        single.setColorAttachments(1, formats);
        single.setDepthAttachment(FramebufferDepth::NONE);
        single.create();
        ASSERT_TRUE(single.isCreated());
        EXPECT_EQ(pixelFormatOf(single.getRender().getColorTexture()), sokolFormatOf(format));
        single.destroy();
        // MRT: a half-float attachment next to RGBA8
        Framebuffer mrt;
        mrt.setWidth(8);
        mrt.setHeight(8);
        mrt.setColorAttachments(2, formats);
        mrt.create();
        ASSERT_TRUE(mrt.isCreated());
        EXPECT_EQ(pixelFormatOf(mrt.getRender().getColorAttachmentTexture(0)), sokolFormatOf(format));
        EXPECT_EQ(pixelFormatOf(mrt.getRender().getColorAttachmentTexture(1)), SG_PIXELFORMAT_RGBA8);
        mrt.destroy();
    }
}

// rendered into (additively, past 1), sampled with a linear filter, the missing channels (0, 1)
TEST(HalfFloatTargets, RenderBlendAndFilter){
    JASPER_REQUIRE_GPU();
    for (ColorFormat format : {ColorFormat::RG16F, ColorFormat::R16F}){
        SCOPED_TRACE(nameOf(format));
        const HalfRun run = renderThroughHalfTarget(format);
        ASSERT_TRUE(run.ok) << run.error;
        ASSERT_EQ(run.rgba.size(), 4u);
        // R: (0 + 2) / 2 x 0.25 = 0.25 (an unorm target would clamp the 2 to 1: 32)
        EXPECT_NEAR(run.rgba[0], 64, 1);
        // G: (2 x 128/255 + 0) / 2 x 0.25 (R16F has no G: 0)
        EXPECT_NEAR(run.rgba[1], format == ColorFormat::RG16F ? 32 : 0, 1);
        // B: not stored, reads 0; A: not stored, reads 1 (x 0.25)
        EXPECT_NEAR(run.rgba[2], 0, 1);
        EXPECT_NEAR(run.rgba[3], 64, 1);
    }
}
