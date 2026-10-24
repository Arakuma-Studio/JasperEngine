// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The dynamic pipeline state the HTML UI renderer and UI effects draw with; SokolPipelineCache
// keys its pipelines by hash(), so equal states must hash equally and every field must count.

#include "render/PipelineState.h"

#include <gtest/gtest.h>

#include <functional>
#include <set>
#include <vector>

using namespace jasperengine;

TEST(PipelineState, Defaults){
    const PipelineState state;
    EXPECT_FALSE(state.target.swapchain);
    EXPECT_EQ(state.target.color, ColorFormat::RGBA);
    EXPECT_EQ(state.target.sampleCount, 1);
    EXPECT_EQ(state.blend, BlendMode::PREMULTIPLIED);
    EXPECT_EQ(state.writeMask, ColorWriteMask::RGBA);
    EXPECT_FALSE(state.stencil.enabled);
    EXPECT_EQ(state.depthCompare, CompareFunc::ALWAYS);
    EXPECT_FALSE(state.depthWrite);
    EXPECT_FALSE(state.cullBack);
}

TEST(PipelineState, EqualStatesHashEqually){
    PipelineState a;
    PipelineState b;
    EXPECT_TRUE(a == b);
    EXPECT_EQ(a.hash(), b.hash());
    a.blend = BlendMode::ALPHA;
    b.blend = BlendMode::ALPHA;
    a.stencil.ref = 3;
    b.stencil.ref = 3;
    EXPECT_TRUE(a == b);
    EXPECT_EQ(a.hash(), b.hash());
}

TEST(PipelineState, EveryFieldChangesEqualityAndHash){
    const std::vector<std::function<void(PipelineState&)>> changes = {
        [](PipelineState& s){ s.target.swapchain = true; },
        [](PipelineState& s){ s.target.color = ColorFormat::SRGB8A8; },
        [](PipelineState& s){ s.target.depth = DepthStencilFormat::NONE; },
        [](PipelineState& s){ s.target.sampleCount = 4; },
        [](PipelineState& s){ s.blend = BlendMode::NONE; },
        [](PipelineState& s){ s.writeMask = ColorWriteMask::RGB; },
        [](PipelineState& s){ s.stencil.enabled = true; },
        [](PipelineState& s){ s.stencil.compare = CompareFunc::EQUAL; },
        [](PipelineState& s){ s.stencil.failOp = StencilOp::ZERO; },
        [](PipelineState& s){ s.stencil.depthFailOp = StencilOp::REPLACE; },
        [](PipelineState& s){ s.stencil.passOp = StencilOp::INVERT; },
        [](PipelineState& s){ s.stencil.readMask = 0x0F; },
        [](PipelineState& s){ s.stencil.writeMask = 0xF0; },
        [](PipelineState& s){ s.stencil.ref = 1; },
        [](PipelineState& s){ s.depthCompare = CompareFunc::LESS; },
        [](PipelineState& s){ s.depthWrite = true; },
        [](PipelineState& s){ s.cullBack = true; },
    };
    const PipelineState base;
    std::set<uint64_t> hashes{base.hash()};
    for (size_t i = 0; i < changes.size(); i++){
        PipelineState changed;
        changes[i](changed);
        EXPECT_TRUE(changed != base) << "change " << i;
        EXPECT_NE(changed.hash(), base.hash()) << "change " << i;
        hashes.insert(changed.hash());
    }
    EXPECT_EQ(hashes.size(), changes.size() + 1);
}

TEST(PipelineState, StencilAndTargetEquality){
    StencilState a;
    StencilState b;
    EXPECT_TRUE(a == b);
    b.writeMask = 1;
    EXPECT_TRUE(a != b);
    RenderTargetFormat x;
    RenderTargetFormat y;
    y.sampleCount = 2;
    EXPECT_TRUE(x != y);
}

TEST(PipelineState, TargetFormatForThePipelineTypes){
    EXPECT_TRUE(targetFormatFor(PipelineType::PIP_DEFAULT).swapchain);
    const RenderTargetFormat rtt = targetFormatFor(PipelineType::PIP_RTT);
    EXPECT_FALSE(rtt.swapchain);
    EXPECT_EQ(rtt.color, ColorFormat::RGBA);
    EXPECT_EQ(rtt.depth, DepthStencilFormat::DEPTH_STENCIL);
    EXPECT_EQ(rtt.sampleCount, 1);
    EXPECT_EQ(targetFormatFor(PipelineType::PIP_MSAA, 4).sampleCount, 4);
    EXPECT_EQ(targetFormatFor(PipelineType::PIP_MSAA, 0).sampleCount, 1);
    const RenderTargetFormat post = targetFormatFor(PipelineType::PIP_POST);
    EXPECT_EQ(post.color, ColorFormat::SRGB8A8);
    EXPECT_EQ(post.depth, DepthStencilFormat::NONE);
}
