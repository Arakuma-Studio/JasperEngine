// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Configurable blend state of the render abstraction (ObjectRender::setBlendState): without
// one, every pipeline type is described as the render state model makes it (the shader
// library's RenderState, SokolObject::makePipelineDesc); with one, only the color pipelines
// change their blending. CPU only (SokolObject::describePipeline).

#include "Engine.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    const PipelineType allTypes[] = {
        PIP_DEFAULT, PIP_RTT, PIP_DEPTH, PIP_RTT_INVERT, PIP_GBUFFER, PIP_SHADOW_DEPTH,
        PIP_DEFAULT_NODEPTH, PIP_RTT_NODEPTH, PIP_ZPREPASS, PIP_ZPREPASS_RTT,
        PIP_EFFECT, PIP_POST, PIP_MSAA, PIP_MSAA_NODEPTH, PIP_ZPREPASS_MSAA, PIP_RTT_COLORONLY
    };

    // the types a BlendState applies to (PIP_POST, the bloom chain's, is never blended)
    bool isColorPipeline(PipelineType type){
        return type == PIP_DEFAULT || type == PIP_DEFAULT_NODEPTH || type == PIP_RTT || type == PIP_RTT_NODEPTH || type == PIP_RTT_INVERT ||
               type == PIP_MSAA || type == PIP_MSAA_NODEPTH || type == PIP_EFFECT;
    }

    // what makePipelineDesc produces for an object loaded with the default RenderState (no
    // face culling, depth writes, less-equal, alpha blend; SokolObject defaults, one sample
    // for the PIP_MSAA* types before an endLoad gives the count). Offscreen colour passes
    // that draw meshes have a depth-stencil attachment, effect targets a depth-only one, the
    // bloom chain's SRGB8A8 targets none
    struct Expected {
        bool blend;
        sg_blend_factor srcRGB, dstRGB, srcAlpha, dstAlpha;
        sg_color_mask writeMask;
        sg_compare_func depthCompare;
        bool depthWrite;
        sg_pixel_format depthFormat;
        sg_pixel_format colorFormat;
        int sampleCount;
        int colorCount;
    };

    Expected expectedFor(PipelineType type){
        const sg_blend_factor none = _SG_BLENDFACTOR_DEFAULT;
        switch (type){
        case PIP_DEPTH:
            return {false, none, none, none, none, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_LESS_EQUAL, true, SG_PIXELFORMAT_DEPTH, SG_PIXELFORMAT_RGBA8, 1, 0};
        case PIP_ZPREPASS:
            return {false, none, none, none, none, SG_COLORMASK_NONE, SG_COMPAREFUNC_LESS_EQUAL, true, _SG_PIXELFORMAT_DEFAULT, _SG_PIXELFORMAT_DEFAULT, 0, 0};
        case PIP_ZPREPASS_RTT:
        case PIP_ZPREPASS_MSAA:
            return {false, none, none, none, none, SG_COLORMASK_NONE, SG_COMPAREFUNC_LESS_EQUAL, true, SG_PIXELFORMAT_DEPTH_STENCIL, SG_PIXELFORMAT_RGBA8, 1, 0};
        case PIP_SHADOW_DEPTH:
            return {false, none, none, none, none, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_LESS_EQUAL, true, SG_PIXELFORMAT_DEPTH, SG_PIXELFORMAT_NONE, 1, 0};
        case PIP_GBUFFER:
            return {false, none, none, none, none, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_LESS_EQUAL, true, SG_PIXELFORMAT_DEPTH, SG_PIXELFORMAT_RGBA8, 1, 3};
        case PIP_DEFAULT:
            return {true, SG_BLENDFACTOR_SRC_ALPHA, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, none, none, SG_COLORMASK_RGB, SG_COMPAREFUNC_LESS_EQUAL, true, _SG_PIXELFORMAT_DEFAULT, _SG_PIXELFORMAT_DEFAULT, 0, 0};
        case PIP_DEFAULT_NODEPTH:
            return {true, SG_BLENDFACTOR_SRC_ALPHA, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, none, none, SG_COLORMASK_RGB, SG_COMPAREFUNC_ALWAYS, false, _SG_PIXELFORMAT_DEFAULT, _SG_PIXELFORMAT_DEFAULT, 0, 0};
        case PIP_RTT:
        case PIP_RTT_INVERT:
        case PIP_MSAA:
            return {true, SG_BLENDFACTOR_SRC_ALPHA, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SG_BLENDFACTOR_ONE, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SG_COLORMASK_RGBA, SG_COMPAREFUNC_LESS_EQUAL, true, SG_PIXELFORMAT_DEPTH_STENCIL, SG_PIXELFORMAT_RGBA8, 1, 0};
        case PIP_RTT_NODEPTH:
        case PIP_MSAA_NODEPTH:
            return {true, SG_BLENDFACTOR_SRC_ALPHA, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SG_BLENDFACTOR_ONE, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SG_COLORMASK_RGBA, SG_COMPAREFUNC_ALWAYS, false, SG_PIXELFORMAT_DEPTH_STENCIL, SG_PIXELFORMAT_RGBA8, 1, 0};
        case PIP_EFFECT:
            return {true, SG_BLENDFACTOR_SRC_ALPHA, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SG_BLENDFACTOR_ONE, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_ALWAYS, false, SG_PIXELFORMAT_DEPTH, SG_PIXELFORMAT_RGBA8, 1, 0};
        case PIP_POST:
            return {false, none, none, none, none, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_ALWAYS, false, SG_PIXELFORMAT_NONE, SG_PIXELFORMAT_SRGB8A8, 1, 0};
        case PIP_RTT_COLORONLY:
            return {false, none, none, none, none, _SG_COLORMASK_DEFAULT, SG_COMPAREFUNC_ALWAYS, false, SG_PIXELFORMAT_NONE, SG_PIXELFORMAT_RGBA8, 1, 0};
        }
        return {};
    }

    ::testing::AssertionResult matches(PipelineType type, const sg_pipeline_desc& desc, const Expected& e){
        const sg_blend_state& blend = desc.colors[0].blend;
        if (blend.enabled != e.blend || blend.src_factor_rgb != e.srcRGB || blend.dst_factor_rgb != e.dstRGB ||
            blend.src_factor_alpha != e.srcAlpha || blend.dst_factor_alpha != e.dstAlpha ||
            blend.op_rgb != _SG_BLENDOP_DEFAULT || blend.op_alpha != _SG_BLENDOP_DEFAULT)
            return ::testing::AssertionFailure() << "blend of type " << int(type);
        if (desc.colors[0].write_mask != e.writeMask)
            return ::testing::AssertionFailure() << "write mask of type " << int(type) << ": " << int(desc.colors[0].write_mask);
        if (desc.depth.compare != e.depthCompare || desc.depth.write_enabled != e.depthWrite || desc.depth.pixel_format != e.depthFormat)
            return ::testing::AssertionFailure() << "depth state of type " << int(type);
        if (desc.colors[0].pixel_format != e.colorFormat || desc.sample_count != e.sampleCount || desc.color_count != e.colorCount)
            return ::testing::AssertionFailure() << "formats of type " << int(type);
        // only the z-prepass pipelines cull without face culling at load (prepassFaceCulling
        // defaults to true). Every type sets its winding (it decides gl_FrontFacing): the load's
        // (CCW) for the swapchain and depth-only passes, the other way on GL for an offscreen
        // pass (Y-flipped there), once more for a planar reflection
        sg_cull_mode cull = _SG_CULLMODE_DEFAULT;
        if (type == PIP_ZPREPASS || type == PIP_ZPREPASS_RTT || type == PIP_ZPREPASS_MSAA)
            cull = SG_CULLMODE_BACK;
        const sg_face_winding offscreen = Engine::isOpenGL() ? SG_FACEWINDING_CW : SG_FACEWINDING_CCW;
        sg_face_winding winding = SG_FACEWINDING_CCW;
        switch (type){
        case PIP_ZPREPASS_RTT: case PIP_ZPREPASS_MSAA: case PIP_RTT: case PIP_RTT_NODEPTH: case PIP_MSAA: case PIP_MSAA_NODEPTH:
        case PIP_EFFECT: case PIP_POST: case PIP_RTT_COLORONLY:
            winding = offscreen;
            break;
        case PIP_RTT_INVERT:
            winding = (offscreen == SG_FACEWINDING_CW) ? SG_FACEWINDING_CCW : SG_FACEWINDING_CW;
            break;
        default:
            break;
        }
        if (desc.cull_mode != cull || desc.face_winding != winding)
            return ::testing::AssertionFailure() << "face state of type " << int(type);
        if (desc.primitive_type != SG_PRIMITIVETYPE_TRIANGLES)
            return ::testing::AssertionFailure() << "primitive type of type " << int(type);
        return ::testing::AssertionSuccess();
    }

    bool sameDescription(const sg_pipeline_desc& a, const sg_pipeline_desc& b){
        if (a.color_count != b.color_count || a.sample_count != b.sample_count || a.primitive_type != b.primitive_type ||
            a.index_type != b.index_type || a.cull_mode != b.cull_mode || a.face_winding != b.face_winding)
            return false;
        if (a.depth.pixel_format != b.depth.pixel_format || a.depth.compare != b.depth.compare || a.depth.write_enabled != b.depth.write_enabled)
            return false;
        for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
            const sg_color_target_state& ca = a.colors[i];
            const sg_color_target_state& cb = b.colors[i];
            if (ca.pixel_format != cb.pixel_format || ca.write_mask != cb.write_mask ||
                ca.blend.enabled != cb.blend.enabled || ca.blend.src_factor_rgb != cb.blend.src_factor_rgb ||
                ca.blend.dst_factor_rgb != cb.blend.dst_factor_rgb || ca.blend.op_rgb != cb.blend.op_rgb ||
                ca.blend.src_factor_alpha != cb.blend.src_factor_alpha || ca.blend.dst_factor_alpha != cb.blend.dst_factor_alpha ||
                ca.blend.op_alpha != cb.blend.op_alpha)
                return false;
        }
        return true;
    }

}

TEST(BlendStateTest, DefaultUnchanged){
    ObjectRender object;
    object.beginLoad(PrimitiveType::TRIANGLES);

    for (PipelineType type : allTypes){
        EXPECT_TRUE(matches(type, object.backend.describePipeline(type), expectedFor(type)));
    }

    // the defaults of BlendState are the RTT pipelines' built-in blending
    BlendState defaults;
    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.srcFactorRGB, BlendFactor::SRC_ALPHA);
    EXPECT_EQ(defaults.dstFactorRGB, BlendFactor::ONE_MINUS_SRC_ALPHA);
    EXPECT_EQ(defaults.srcFactorAlpha, BlendFactor::ONE);
    EXPECT_EQ(defaults.dstFactorAlpha, BlendFactor::ONE_MINUS_SRC_ALPHA);
}

TEST(BlendStateTest, PremultipliedOverride){
    ObjectRender plain;
    plain.beginLoad(PrimitiveType::TRIANGLES);

    ObjectRender premultiplied;
    premultiplied.beginLoad(PrimitiveType::TRIANGLES);
    premultiplied.setBlendState(BlendState::premultipliedAlpha());

    for (PipelineType type : allTypes){
        const sg_pipeline_desc desc = premultiplied.backend.describePipeline(type);
        const sg_pipeline_desc reference = plain.backend.describePipeline(type);
        if (isColorPipeline(type)){
            const sg_blend_state& blend = desc.colors[0].blend;
            EXPECT_TRUE(blend.enabled) << int(type);
            EXPECT_EQ(blend.src_factor_rgb, SG_BLENDFACTOR_ONE) << int(type);
            EXPECT_EQ(blend.dst_factor_rgb, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA) << int(type);
            EXPECT_EQ(blend.op_rgb, SG_BLENDOP_ADD) << int(type);
            EXPECT_EQ(blend.src_factor_alpha, SG_BLENDFACTOR_ONE) << int(type);
            EXPECT_EQ(blend.dst_factor_alpha, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA) << int(type);
            EXPECT_EQ(blend.op_alpha, SG_BLENDOP_ADD) << int(type);
            // everything but the blending is the pipeline type's own
            EXPECT_EQ(desc.colors[0].write_mask, reference.colors[0].write_mask) << int(type);
            EXPECT_EQ(desc.depth.compare, reference.depth.compare) << int(type);
            EXPECT_EQ(desc.depth.write_enabled, reference.depth.write_enabled) << int(type);
            EXPECT_EQ(desc.colors[0].pixel_format, reference.colors[0].pixel_format) << int(type);
        }else{
            // depth, shadow, G-buffer, z-prepass and bloom-chain pipelines ignore it
            EXPECT_TRUE(sameDescription(desc, reference)) << int(type);
        }
    }

    // beginLoad forgets it
    premultiplied.beginLoad(PrimitiveType::TRIANGLES);
    for (PipelineType type : allTypes){
        EXPECT_TRUE(matches(type, premultiplied.backend.describePipeline(type), expectedFor(type)));
    }
}

TEST(BlendStateTest, OpaqueDisablesBlend){
    ObjectRender object;
    object.beginLoad(PrimitiveType::TRIANGLES);
    object.setBlendState(BlendState::opaque());

    ObjectRender plain;
    plain.beginLoad(PrimitiveType::TRIANGLES);

    for (PipelineType type : allTypes){
        const sg_pipeline_desc desc = object.backend.describePipeline(type);
        if (isColorPipeline(type)){
            EXPECT_FALSE(desc.colors[0].blend.enabled) << int(type);
        }else{
            EXPECT_TRUE(sameDescription(desc, plain.backend.describePipeline(type))) << int(type);
        }
    }

    // a copy keeps it (render objects are handles copied around)
    ObjectRender copy = object;
    EXPECT_FALSE(copy.backend.describePipeline(PIP_RTT).colors[0].blend.enabled);
}

TEST(BlendStateTest, CustomFactorsAndOperations){
    BlendState state;
    state.srcFactorRGB = BlendFactor::DST_COLOR;
    state.dstFactorRGB = BlendFactor::ZERO;
    state.opRGB = BlendOperation::REVERSE_SUBTRACT;
    state.srcFactorAlpha = BlendFactor::ONE_MINUS_DST_ALPHA;
    state.dstFactorAlpha = BlendFactor::SRC_COLOR;
    state.opAlpha = BlendOperation::SUBTRACT;

    ObjectRender object;
    object.beginLoad(PrimitiveType::TRIANGLES);
    object.setBlendState(state);

    const sg_blend_state& blend = object.backend.describePipeline(PIP_DEFAULT).colors[0].blend;
    EXPECT_EQ(blend.src_factor_rgb, SG_BLENDFACTOR_DST_COLOR);
    EXPECT_EQ(blend.dst_factor_rgb, SG_BLENDFACTOR_ZERO);
    EXPECT_EQ(blend.op_rgb, SG_BLENDOP_REVERSE_SUBTRACT);
    EXPECT_EQ(blend.src_factor_alpha, SG_BLENDFACTOR_ONE_MINUS_DST_ALPHA);
    EXPECT_EQ(blend.dst_factor_alpha, SG_BLENDFACTOR_SRC_COLOR);
    EXPECT_EQ(blend.op_alpha, SG_BLENDOP_SUBTRACT);
}

// targetFormatFor (the HTML UI's camera-pass formats) says what the fixed types are built with
TEST(BlendStateTest, TargetFormatsMatchPipelines){
    ObjectRender object;
    object.beginLoad(PrimitiveType::TRIANGLES);
    for (PipelineType type : {PIP_RTT, PIP_RTT_NODEPTH, PIP_RTT_INVERT, PIP_ZPREPASS_RTT, PIP_EFFECT, PIP_POST,
                              PIP_MSAA, PIP_MSAA_NODEPTH, PIP_ZPREPASS_MSAA, PIP_RTT_COLORONLY}){
        const sg_pipeline_desc desc = object.backend.describePipeline(type);
        const RenderTargetFormat format = targetFormatFor(type);
        EXPECT_FALSE(format.swapchain) << int(type);
        const sg_pixel_format depth = format.depth == DepthStencilFormat::DEPTH_STENCIL ? SG_PIXELFORMAT_DEPTH_STENCIL :
                                      format.depth == DepthStencilFormat::DEPTH ? SG_PIXELFORMAT_DEPTH : SG_PIXELFORMAT_NONE;
        EXPECT_EQ(desc.depth.pixel_format, depth) << int(type);
        EXPECT_EQ(desc.colors[0].pixel_format, format.color == ColorFormat::SRGB8A8 ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8) << int(type);
        EXPECT_EQ(desc.sample_count, format.sampleCount) << int(type);
    }
    for (PipelineType type : {PIP_DEFAULT, PIP_DEFAULT_NODEPTH, PIP_ZPREPASS})
        EXPECT_TRUE(targetFormatFor(type).swapchain) << int(type);
    // the multisampled scene target's count
    EXPECT_EQ(targetFormatFor(PIP_MSAA_NODEPTH, 4).sampleCount, 4);
    EXPECT_EQ(targetFormatFor(PIP_MSAA_NODEPTH, 4).depth, DepthStencilFormat::DEPTH_STENCIL);
    EXPECT_EQ(targetFormatFor(PIP_RTT_NODEPTH, 4).sampleCount, 1);
}
