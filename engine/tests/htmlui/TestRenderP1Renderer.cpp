// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-1 render abstraction and renderer (spec §5.1 "Render"): PipelineState, the pipeline
// cache, MSAA framebuffers with resolve and a shared depth-stencil, CompileFilter/CompileShader
// against RmlUi's GL3 formulas, the blur parameters, pass counts, the stencil reference of
// nested clips, steady-state allocations, the straight-alpha copy and the sample count choice.

#include "TestRenderCommon.h"

#include "harness/SceneFixture.h"

#include "HtmlUIClock.h"
#include "HtmlUIContext.h"
#include "HtmlUITexture.h"
#include "RmlFilterContext.h"

#include "pool/ShaderPool.h"
#include "render/FramebufferRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "sokol/SokolPipelineCache.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DecorationTypes.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const float pi = 3.14159265358979f;

    // the colour matrices of RmlUi's GL3 renderer (CompileFilter), written out by rows
    struct Rows { float m[4][4]; };

    Rows identityRows(){
        Rows r = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
        return r;
    }

    Rows expectedMatrix(const std::string& name, float v){
        Rows r = identityRows();
        if (name == "brightness"){
            r.m[0][0] = r.m[1][1] = r.m[2][2] = v;
        }else if (name == "contrast"){
            const float g = 0.5f - 0.5f * v;
            r.m[0][0] = r.m[1][1] = r.m[2][2] = v;
            r.m[0][3] = r.m[1][3] = r.m[2][3] = g;
        }else if (name == "invert"){
            const float i = 1.0f - 2.0f * v;
            r.m[0][0] = r.m[1][1] = r.m[2][2] = i;
            r.m[0][3] = r.m[1][3] = r.m[2][3] = v;
        }else if (name == "grayscale"){
            const float rev = 1.0f - v;
            const float g[3] = {0.2126f * v, 0.7152f * v, 0.0722f * v};
            for (int row = 0; row < 3; row++)
                for (int c = 0; c < 3; c++)
                    r.m[row][c] = g[c] + (row == c ? rev : 0.0f);
        }else if (name == "sepia"){
            const float rev = 1.0f - v;
            const float s[3][3] = {{0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}};
            for (int row = 0; row < 3; row++)
                for (int c = 0; c < 3; c++)
                    r.m[row][c] = v * s[row][c] + (row == c ? rev : 0.0f);
        }else if (name == "hue-rotate"){
            const float s = std::sin(v), c = std::cos(v);
            const float h[3][3] = {
                {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s},
                {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s},
                {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s}};
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    r.m[row][col] = h[row][col];
        }else if (name == "saturate"){
            const float t[3][3] = {
                {0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v},
                {0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v},
                {0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v}};
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    r.m[row][col] = t[row][col];
        }
        return r;
    }

    // HtmlUI (RmlUi with the engine's interfaces) for the CPU-only tests, which read the
    // [HTML UI] warnings RmlUi logs
    struct HtmlUIScope {
        bool acquired = HtmlUI::acquire();
        ~HtmlUIScope(){ if (acquired) HtmlUI::release(); }
    };

    Rml::ColorStopList stops(int count){
        Rml::ColorStopList list;
        for (int i = 0; i < count; i++){
            Rml::ColorStop stop;
            stop.color = Rml::ColourbPremultiplied((Rml::byte)(i * 10), 0, 0, 255);
            stop.position = Rml::NumericValue(count > 1 ? (float)i / (float)(count - 1) : 0.0f, Rml::Unit::NUMBER);
            list.push_back(stop);
        }
        return list;
    }

    // a renderer that records the stencil reference after every clip mask operation
    class RecordingClipRenderer : public RmlRenderer {
    public:
        std::vector<Rml::ClipMaskOperation> operations;
        std::vector<int> refs;
        void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override{
            RmlRenderer::RenderToClipMask(operation, geometry, translation);
            operations.push_back(operation);
            refs.push_back(getStencilRef());
        }
    };

}

//-------------------------------------------------------------------------------------------
// PipelineState and the pipeline cache
//-------------------------------------------------------------------------------------------

TEST(RenderP1Test, PipelineStateHashCoversEveryField){
    const PipelineState base;
    EXPECT_EQ(base.hash(), PipelineState().hash());
    EXPECT_TRUE(base == PipelineState());

    std::vector<PipelineState> variants;
    auto vary = [&](std::function<void(PipelineState&)> change){
        PipelineState state;
        change(state);
        variants.push_back(state);
    };
    vary([](PipelineState& s){ s.target.swapchain = true; });
    vary([](PipelineState& s){ s.target.color = ColorFormat::RED; });
    vary([](PipelineState& s){ s.target.depth = DepthStencilFormat::NONE; });
    vary([](PipelineState& s){ s.target.sampleCount = 4; });
    vary([](PipelineState& s){ s.blend = BlendMode::NONE; });
    vary([](PipelineState& s){ s.blend = BlendMode::SCREEN_PREMULTIPLIED; });
    vary([](PipelineState& s){ s.writeMask = ColorWriteMask::NONE; });
    vary([](PipelineState& s){ s.stencil.enabled = true; });
    vary([](PipelineState& s){ s.stencil.compare = CompareFunc::EQUAL; });
    vary([](PipelineState& s){ s.stencil.failOp = StencilOp::ZERO; });
    vary([](PipelineState& s){ s.stencil.depthFailOp = StencilOp::INVERT; });
    vary([](PipelineState& s){ s.stencil.passOp = StencilOp::REPLACE; });
    vary([](PipelineState& s){ s.stencil.readMask = 0x0F; });
    vary([](PipelineState& s){ s.stencil.writeMask = 0x00; });
    vary([](PipelineState& s){ s.stencil.ref = 1; });
    vary([](PipelineState& s){ s.depthCompare = CompareFunc::LESS; });
    vary([](PipelineState& s){ s.depthWrite = true; });
    vary([](PipelineState& s){ s.cullBack = true; });

    for (size_t i = 0; i < variants.size(); i++){
        SCOPED_TRACE(i);
        EXPECT_FALSE(variants[i] == base);
        EXPECT_NE(variants[i].hash(), base.hash());
        for (size_t j = i + 1; j < variants.size(); j++){
            EXPECT_NE(variants[i].hash(), variants[j].hash()) << i << " " << j;
        }
    }

    // the camera-pass formats of the fixed pipeline types (offscreen camera passes have a
    // depth-stencil attachment since the shader library's stencil work; BlendStateTest
    // compares every type with its pipeline description)
    EXPECT_TRUE(targetFormatFor(PIP_DEFAULT).swapchain);
    EXPECT_TRUE(targetFormatFor(PIP_DEFAULT_NODEPTH).swapchain);
    const RenderTargetFormat rtt = targetFormatFor(PIP_RTT_NODEPTH);
    EXPECT_FALSE(rtt.swapchain);
    EXPECT_EQ(rtt.depth, DepthStencilFormat::DEPTH_STENCIL);
    EXPECT_EQ(rtt.sampleCount, 1);
}

TEST(RenderP1Test, PipelineCacheByState){
    JASPER_REQUIRE_GPU();
    std::shared_ptr<ShaderRender> shader;
    for (int i = 0; i < 30 && !(shader && shader->isCreated()); i++){
        beginTestFrame();
        shader = ShaderPool::get(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH));
    }
    ASSERT_TRUE(shader && shader->isCreated());

    sg_pipeline_desc base = {};
    base.shader = shader->backend.get();
    base.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;

    PipelineState state;
    state.target.depth = DepthStencilFormat::DEPTH_STENCIL;
    state.target.sampleCount = 1;
    state.stencil.enabled = true;
    state.stencil.compare = CompareFunc::EQUAL;
    state.stencil.ref = 7;

    const size_t before = SokolPipelineCache::size();
    const uint64_t misses = SokolPipelineCache::getMissCount();
    sg_pipeline first = SokolPipelineCache::get(base, state);
    ASSERT_NE(first.id, SG_INVALID_ID);
    EXPECT_EQ(SokolPipelineCache::get(base, state).id, first.id);
    EXPECT_EQ(SokolPipelineCache::getMissCount(), misses + 1);

    PipelineState other = state;
    other.stencil.ref = 8;
    sg_pipeline second = SokolPipelineCache::get(base, other);
    EXPECT_NE(second.id, first.id);
    EXPECT_EQ(SokolPipelineCache::size(), before + 2);

    // what the description carries
    const sg_pipeline_desc desc = SokolPipelineCache::makeDesc(base, other);
    EXPECT_EQ(desc.stencil.ref, 8);
    EXPECT_EQ(desc.stencil.front.compare, SG_COMPAREFUNC_EQUAL);
    EXPECT_EQ(desc.depth.pixel_format, SG_PIXELFORMAT_DEPTH_STENCIL);
    EXPECT_EQ(desc.colors[0].blend.src_factor_rgb, SG_BLENDFACTOR_ONE);
    EXPECT_EQ(desc.colors[0].blend.dst_factor_rgb, SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
    PipelineState none = state;
    none.writeMask = ColorWriteMask::NONE;
    none.blend = BlendMode::MULTIPLY_PREMULTIPLIED;
    const sg_pipeline_desc noneDesc = SokolPipelineCache::makeDesc(base, none);
    EXPECT_EQ(noneDesc.colors[0].write_mask, SG_COLORMASK_NONE);
    EXPECT_EQ(noneDesc.colors[0].blend.src_factor_rgb, SG_BLENDFACTOR_DST_COLOR);

    // the shader's destruction takes its pipelines along
    const size_t withBoth = SokolPipelineCache::size();
    SokolPipelineCache::releaseShader(shader->backend.get());
    EXPECT_LE(SokolPipelineCache::size(), withBoth - 2);
    EXPECT_EQ(sg_query_pipeline_state(first), SG_RESOURCESTATE_INVALID);
    const uint64_t missesBefore = SokolPipelineCache::getMissCount();
    SokolPipelineCache::get(base, state);
    EXPECT_EQ(SokolPipelineCache::getMissCount(), missesBefore + 1);
    SokolPipelineCache::releaseShader(shader->backend.get());
}

//-------------------------------------------------------------------------------------------
// Framebuffers
//-------------------------------------------------------------------------------------------

TEST(RenderP1Test, FramebufferMsaaResolveAndSharedDepthStencil){
    JASPER_REQUIRE_GPU();
    const RenderFeatures& features = SystemRender::getFeatures();
    EXPECT_EQ(features.originTopLeft, !Engine::isOpenGL());
    ASSERT_TRUE(features.msaaRGBA8 && features.msaaDepthStencil) << "the test devices multisample";

    FramebufferDesc desc;
    desc.width = 64;
    desc.height = 32;
    desc.sampleCount = 4;
    desc.label = "test-msaa";
    Framebuffer first;
    ASSERT_TRUE(first.create(desc));
    FramebufferRender& render = first.getRender();
    EXPECT_EQ(render.getSampleCount(), 4);
    // the sampled texture is the 1-sample resolve image
    const sg_image resolve = render.getColorTexture().backend.get();
    EXPECT_EQ(sg_query_image_desc(resolve).sample_count, 1);
    EXPECT_TRUE(sg_query_image_desc(resolve).usage.resolve_attachment);
    sg_attachments attachments = render.backend.get(0);
    EXPECT_NE(attachments.resolves[0].id, SG_INVALID_ID);
    EXPECT_NE(attachments.depth_stencil.id, SG_INVALID_ID);
    EXPECT_EQ(sg_query_image_desc(sg_query_view_image(attachments.colors[0])).sample_count, 4);
    EXPECT_EQ(sg_query_image_desc(sg_query_view_image(attachments.depth_stencil)).pixel_format, SG_PIXELFORMAT_DEPTH_STENCIL);

    // a second one borrows the first one's depth-stencil attachment
    FramebufferDesc shared = desc;
    shared.shareDepthStencil = &render;
    Framebuffer second;
    ASSERT_TRUE(second.create(shared));
    EXPECT_EQ(second.getRender().backend.get(0).depth_stencil.id, attachments.depth_stencil.id);
    // a different sample count cannot borrow it
    TestSystem::get().clearLogs();
    FramebufferDesc mismatch = shared;
    mismatch.sampleCount = 1;
    Framebuffer third;
    EXPECT_FALSE(third.create(mismatch));
    // destroying the borrower keeps the owner's attachment
    second.destroy();
    EXPECT_TRUE(first.isCreated());
    first.destroy();

    // one sample: the colour attachment is the sampled texture, no resolve
    FramebufferDesc single = desc;
    single.sampleCount = 1;
    single.depth = DepthStencilFormat::NONE;
    Framebuffer plain;
    ASSERT_TRUE(plain.create(single));
    EXPECT_EQ(plain.getRender().backend.get(0).resolves[0].id, SG_INVALID_ID);
    EXPECT_EQ(plain.getRender().backend.get(0).depth_stencil.id, SG_INVALID_ID);
    EXPECT_EQ(plain.getVersion(), 1u);
    plain.destroy();
}

TEST(RenderP1Test, SampleCountChoice){
    JASPER_REQUIRE_GPU();
    const int initial = UIDocumentSystem::getPreferredSampleCount();
    EXPECT_EQ(initial, 4);
    HtmlUIContext context("sample-count");
    EXPECT_EQ(context.getSampleCount(), 4);

    UIDocumentSystem::setPreferredSampleCount(1);
    EXPECT_EQ(context.getSampleCount(), 1);
    UIDocumentSystem::setPreferredSampleCount(2);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 2);
    // not 1/2/4/8: the nearest below, logged
    TestSystem::get().clearLogs();
    UIDocumentSystem::setPreferredSampleCount(6);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 4);
    UIDocumentSystem::setPreferredSampleCount(16);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 8);
    UIDocumentSystem::setPreferredSampleCount(0);
    EXPECT_EQ(UIDocumentSystem::getPreferredSampleCount(), 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] MSAA sample count"), 3u);

    // 8 where the device has it, else the highest count below (lavapipe and llvmpipe: 4); a
    // target asked for more samples than the device has is made with that count
    UIDocumentSystem::setPreferredSampleCount(8);
    const int device = FramebufferRender::getMaxMsaaSamples(8);
    EXPECT_GE(device, 1);
    EXPECT_LE(device, 8);
    EXPECT_EQ(context.getSampleCount(), device);
    RmlRenderTarget target;
    target.ensure(16, 16, 8);
    EXPECT_EQ(target.getSampleCount(), device);
    EXPECT_TRUE(target.isCreated());

    UIDocumentSystem::setPreferredSampleCount(initial);
}

// The composite inside the post-processed camera's multisampled scene target (the shader
// library's MSAA, CameraRender::setMultisample): PIP_MSAA_NODEPTH through the pipeline cache at
// the target's sample count draws the same pixels as the one-sample composite
TEST(RenderP1Test, CompositeIntoMultisampledSceneTarget){
    JASPER_REQUIRE_GPU();
    const int width = 160, height = 90;
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    const std::string rml = "<html><head><style>body { width: 100%; height: 100%; }"
                            " #a { position: absolute; left: 10px; top: 8px; width: 70px; height: 40px; background-color: #e04030;"
                            " border-radius: 12px; }"
                            " #b { position: absolute; left: 60px; top: 30px; width: 80px; height: 50px; background-color: #2080ffa0; }"
                            "</style></head><body><div id=\"a\"/><div id=\"b\"/></body></html>";
    ASSERT_NE(test.loadFromMemory(rml, "p1/msaa-scene-composite.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    std::vector<uint8_t> single;
    ASSERT_TRUE(test.readScreen(single));

    const int samples = FramebufferRender::getMaxMsaaSamples(4);
    ASSERT_GT(samples, 1) << "the test devices multisample";
    FramebufferRender msaa;
    ASSERT_TRUE(msaa.createMultisampleFramebuffer(width, height, samples));
    FramebufferRender scene;
    ASSERT_TRUE(scene.createFramebuffer(TextureType::TEXTURE_2D, width, height, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                        TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false));

    const uint64_t missesBefore = SystemRender::getPipelineCacheMisses();
    bool drawn = false;
    for (int frame = 0; frame < 2; frame++){
        CameraRender pass;
        const float bg = RenderTestContext::background / 255.0f;
        pass.setClearColor(Vector4(bg, bg, bg, 1.0f));
        pass.setMultisample(&msaa);
        pass.startRenderPass(&scene);
        const Rect full(0, 0, (float)width, (float)height);
        pass.applyViewport(full);
        pass.applyScissor(full);
        // an offscreen camera pass renders Y-flipped on GL (RenderSystem::isRenderingFlipped)
        const Rml::Matrix4f clipFromQuad = Engine::isOpenGL() ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity();
        drawn = test.renderer.drawComposite(test.target.getComposited()->getColorTexture(), PIP_MSAA_NODEPTH, clipFromQuad, samples);
        pass.endRenderPass();
        SystemRender::commit();
        EXPECT_TRUE(drawn) << "frame " << frame;
    }
    // one pipeline for the multisampled composite, made once
    EXPECT_EQ(SystemRender::getPipelineCacheMisses() - missesBefore, 1u);

    std::vector<uint8_t> resolved;
    ASSERT_TRUE(readTexture(scene.getColorTexture(), width, height, resolved));
    ASSERT_EQ(resolved.size(), single.size());
    int worst = 0;
    for (size_t i = 0; i < resolved.size(); i++)
        worst = std::max(worst, std::abs((int)resolved[i] - (int)single[i]));
    EXPECT_LE(worst, 1) << "the resolved composite differs from the one-sample one";

    msaa.destroyFramebuffer();
    scene.destroyFramebuffer();
}

//-------------------------------------------------------------------------------------------
// CompileFilter, CompileShader, blur parameters (CPU)
//-------------------------------------------------------------------------------------------

TEST(RenderP1Test, ColorMatrixFiltersMatchGL3){
    HtmlUIScope html;
    ASSERT_TRUE(html.acquired);
    RmlRenderer renderer;

    const char* names[] = {"brightness", "contrast", "invert", "grayscale", "sepia", "hue-rotate", "saturate"};
    for (const char* name : names){
        for (float value : {0.0f, 0.5f, 1.0f}){
            SCOPED_TRACE(std::string(name) + " " + std::to_string(value));
            Rml::CompiledFilterHandle handle = renderer.CompileFilter(name, Rml::Dictionary{{"value", Rml::Variant(value)}});
            ASSERT_NE(handle, 0u);
            EXPECT_EQ(handle & 1, 0u) << "bit 0 is phase 5's";
            const RmlCompiledFilter& filter = *reinterpret_cast<const RmlCompiledFilter*>(handle);
            EXPECT_EQ(filter.type, RmlCompiledFilter::Type::COLOR_MATRIX);
            const Rows expected = expectedMatrix(name, value);
            for (int row = 0; row < 4; row++){
                for (int col = 0; col < 4; col++){
                    // Rml::Matrix4f is column-major: column col, row row
                    EXPECT_NEAR(filter.colorMatrix[col][row], expected.m[row][col], 1e-6) << row << ", " << col;
                }
            }
            renderer.ReleaseFilter(handle);
        }
    }
}

TEST(RenderP1Test, FiltersMap){
    HtmlUIScope html;
    ASSERT_TRUE(html.acquired);
    RmlRenderer renderer;

    Rml::CompiledFilterHandle opacity = renderer.CompileFilter("opacity", Rml::Dictionary{{"value", Rml::Variant(0.25f)}});
    ASSERT_NE(opacity, 0u);
    EXPECT_EQ(reinterpret_cast<const RmlCompiledFilter*>(opacity)->type, RmlCompiledFilter::Type::PASSTHROUGH);
    EXPECT_FLOAT_EQ(reinterpret_cast<const RmlCompiledFilter*>(opacity)->blendFactor, 0.25f);

    Rml::CompiledFilterHandle blur = renderer.CompileFilter("blur", Rml::Dictionary{{"sigma", Rml::Variant(4.0f)}});
    ASSERT_NE(blur, 0u);
    EXPECT_EQ(reinterpret_cast<const RmlCompiledFilter*>(blur)->type, RmlCompiledFilter::Type::BLUR);
    EXPECT_FLOAT_EQ(reinterpret_cast<const RmlCompiledFilter*>(blur)->sigma, 4.0f);

    Rml::CompiledFilterHandle shadow = renderer.CompileFilter("drop-shadow", Rml::Dictionary{
        {"sigma", Rml::Variant(3.0f)}, {"color", Rml::Variant(Rml::Colourb(255, 0, 0, 128))}, {"offset", Rml::Variant(Rml::Vector2f(4.0f, -2.0f))}});
    ASSERT_NE(shadow, 0u);
    const RmlCompiledFilter& dropShadow = *reinterpret_cast<const RmlCompiledFilter*>(shadow);
    EXPECT_EQ(dropShadow.type, RmlCompiledFilter::Type::DROP_SHADOW);
    EXPECT_FLOAT_EQ(dropShadow.sigma, 3.0f);
    EXPECT_EQ(dropShadow.offset, Rml::Vector2f(4.0f, -2.0f));
    // premultiplied
    EXPECT_EQ(dropShadow.color.red, 128);
    EXPECT_EQ(dropShadow.color.alpha, 128);

    TestSystem::get().clearLogs();
    EXPECT_EQ(renderer.CompileFilter("pixelate", Rml::Dictionary{}), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "pixelate"), 1u);

    renderer.ReleaseFilter(opacity);
    renderer.ReleaseFilter(blur);
    renderer.ReleaseFilter(shadow);
}

TEST(RenderP1Test, GradientShadersMap){
    HtmlUIScope html;
    ASSERT_TRUE(html.acquired);
    RmlRenderer renderer;
    using Function = RmlCompiledShader::Function;

    for (bool repeating : {false, true}){
        SCOPED_TRACE(repeating ? "repeating" : "plain");
        Rml::CompiledShaderHandle linear = renderer.CompileShader("linear-gradient", Rml::Dictionary{
            {"p0", Rml::Variant(Rml::Vector2f(10, 20))}, {"p1", Rml::Variant(Rml::Vector2f(110, 70))},
            {"repeating", Rml::Variant(repeating)}, {"color_stop_list", Rml::Variant(stops(3))}});
        ASSERT_NE(linear, 0u);
        EXPECT_EQ(linear & 1, 0u);
        const RmlCompiledShader& l = *reinterpret_cast<const RmlCompiledShader*>(linear);
        EXPECT_EQ(l.function, repeating ? Function::REPEATING_LINEAR : Function::LINEAR);
        EXPECT_EQ(l.p, Rml::Vector2f(10, 20));
        EXPECT_EQ(l.v, Rml::Vector2f(100, 50));
        ASSERT_EQ(l.stopPositions.size(), 3u);
        EXPECT_FLOAT_EQ(l.stopPositions[1], 0.5f);
        EXPECT_NEAR(l.stopColors[1].red, 10.0f / 255.0f, 1e-6);

        Rml::CompiledShaderHandle radial = renderer.CompileShader("radial-gradient", Rml::Dictionary{
            {"center", Rml::Variant(Rml::Vector2f(50, 40))}, {"radius", Rml::Variant(Rml::Vector2f(25, 10))},
            {"repeating", Rml::Variant(repeating)}, {"color_stop_list", Rml::Variant(stops(2))}});
        ASSERT_NE(radial, 0u);
        const RmlCompiledShader& r = *reinterpret_cast<const RmlCompiledShader*>(radial);
        EXPECT_EQ(r.function, repeating ? Function::REPEATING_RADIAL : Function::RADIAL);
        EXPECT_EQ(r.p, Rml::Vector2f(50, 40));
        EXPECT_EQ(r.v, Rml::Vector2f(1.0f / 25.0f, 1.0f / 10.0f));

        Rml::CompiledShaderHandle conic = renderer.CompileShader("conic-gradient", Rml::Dictionary{
            {"center", Rml::Variant(Rml::Vector2f(30, 30))}, {"angle", Rml::Variant(pi / 2.0f)},
            {"repeating", Rml::Variant(repeating)}, {"color_stop_list", Rml::Variant(stops(2))}});
        ASSERT_NE(conic, 0u);
        const RmlCompiledShader& c = *reinterpret_cast<const RmlCompiledShader*>(conic);
        EXPECT_EQ(c.function, repeating ? Function::REPEATING_CONIC : Function::CONIC);
        EXPECT_EQ(c.p, Rml::Vector2f(30, 30));
        EXPECT_NEAR(c.v.x, 0.0f, 1e-6);
        EXPECT_NEAR(c.v.y, 1.0f, 1e-6);

        renderer.ReleaseShader(linear);
        renderer.ReleaseShader(radial);
        renderer.ReleaseShader(conic);
    }

    // 17 stops: the first 16, with a warning
    TestSystem::get().clearLogs();
    Rml::CompiledShaderHandle many = renderer.CompileShader("linear-gradient", Rml::Dictionary{
        {"p0", Rml::Variant(Rml::Vector2f(0, 0))}, {"p1", Rml::Variant(Rml::Vector2f(100, 0))}, {"color_stop_list", Rml::Variant(stops(17))}});
    ASSERT_NE(many, 0u);
    EXPECT_EQ(reinterpret_cast<const RmlCompiledShader*>(many)->stopPositions.size(), 16u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "17 color stops"), 1u);
    renderer.ReleaseShader(many);

    // any other name (decorator: shader(...) included) draws nothing, with a warning
    TestSystem::get().clearLogs();
    EXPECT_EQ(renderer.CompileShader("shader", Rml::Dictionary{{"value", Rml::Variant(Rml::String("creation"))}}), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "Unsupported shader 'shader'"), 1u);
}

TEST(RenderP1Test, BlurParameters){
    struct Expected { float sigma; int passLevel; float passSigma; };
    // GL3: pass level log2(int(sigma * 2 / 3)) in [0, 10], remaining sigma at most 3
    const Expected table[] = {
        {0.5f, 0, 0.5f}, {1.0f, 0, 1.0f}, {3.0f, 1, 1.5f}, {6.0f, 2, 1.5f},
        {12.0f, 3, 1.5f}, {48.0f, 5, 1.5f}, {400.0f, 8, 1.5625f},
    };
    for (const Expected& e : table){
        SCOPED_TRACE(e.sigma);
        int passLevel = -1;
        float sigma = -1.0f;
        RmlRenderer::sigmaToParameters(e.sigma, passLevel, sigma);
        EXPECT_EQ(passLevel, e.passLevel);
        EXPECT_FLOAT_EQ(sigma, e.passSigma);
    }

    // normalized weights: w0 + 2 (w1 + w2 + w3) = 1, decreasing; a tiny sigma is a copy
    float weights[4];
    RmlRenderer::blurWeights(1.5f, weights);
    EXPECT_NEAR(weights[0] + 2.0f * (weights[1] + weights[2] + weights[3]), 1.0f, 1e-6);
    EXPECT_GT(weights[0], weights[1]);
    EXPECT_GT(weights[1], weights[2]);
    EXPECT_GT(weights[2], weights[3]);
    RmlRenderer::blurWeights(0.05f, weights);
    EXPECT_FLOAT_EQ(weights[0], 1.0f);
    EXPECT_FLOAT_EQ(weights[1], 0.0f);
}

//-------------------------------------------------------------------------------------------
// Frames: passes, clips, allocations, release
//-------------------------------------------------------------------------------------------

TEST(RenderP1Test, PassCounts){
    JASPER_REQUIRE_GPU();
    const char* head = "<html><head><link type=\"text/css\" href=\"../common/base.css\"/><link type=\"text/css\" href=\"p1.css\"/><style>"
                       "#bg { left: 0px; top: 0px; width: 200px; height: 200px; background-color: #405060; }"
                       "#f { left: 40px; top: 40px; width: 100px; height: 100px; background-color: #e04020; }"
                       "#inner { position: absolute; left: 20px; top: 20px; width: 40px; height: 40px; background-color: #20e040; }";

    struct Case { const char* name; const char* style; const char* body; uint32_t passes; uint32_t layers; uint32_t filters; };
    const Case cases[] = {
        // layer 0, one pass
        {"static", "", "<div id=\"bg\" class=\"abs\"/><div id=\"f\" class=\"abs\"/>", 1, 0, 0},
        // + the filtered layer, blur(4px) (sigma 4: one downscale, vertical, horizontal, upscale), the composite back
        {"blur", "#f { filter: blur(4px); }", "<div id=\"bg\" class=\"abs\"/><div id=\"f\" class=\"abs\"/>", 7, 1, 1},
        // the inner opacity layer (its pass, the opacity pass, back into the blur layer) inside it
        {"opacity in blur", "#f { filter: blur(4px); } #inner { filter: opacity(0.5); }",
         "<div id=\"bg\" class=\"abs\"/><div id=\"f\" class=\"abs\"><div id=\"inner\"/></div>", 10, 2, 2},
        // a backdrop blur(4px): its four blur passes, then the output straight into layer 0 again
        // (the pushed layer is skipped, DeferredComposite)
        {"backdrop", "#f { backdrop-filter: blur(4px); background-color: transparent; }", "<div id=\"bg\" class=\"abs\"/><div id=\"f\" class=\"abs\"/>", 6, 1, 1},
    };

    for (const Case& c : cases){
        SCOPED_TRACE(c.name);
        RenderTestContext test(512, 384, 0, 0, 4);
        ASSERT_TRUE(test.isValid());
        const std::string rml = std::string(head) + c.style + "</style></head><body>" + c.body + "</body></html>";
        ASSERT_NE(test.loadFromMemory(rml, "p1/passes.html"), nullptr);
        ASSERT_TRUE(test.renderUntilDrawn());
        const HtmlUIRenderStats& stats = test.renderer.getStats();
        EXPECT_EQ(stats.passes, c.passes);
        EXPECT_EQ(stats.layersPushed, c.layers);
        EXPECT_EQ(stats.filtersApplied, c.filters);
        EXPECT_EQ(stats.pipelineCacheMisses, 0u) << "a steady frame creates no pipeline";
    }
}

// p5-tooling: a backdrop's second composite draws the filter output straight into the destination
// (RmlRenderer's DeferredComposite), skipping the temporary layer's pass and resolve; the image is
// the one the temporary layer gives, byte for byte (with fusion off, as RmlUi's GL3 renderer draws).
TEST(RenderP1Test, BackdropCompositeFusedExactly){
    JASPER_REQUIRE_GPU();
    const char* head = "<html><head><link type=\"text/css\" href=\"../common/base.css\"/><link type=\"text/css\" href=\"p1.css\"/><style>"
                       ".c { position: absolute; width: 60px; height: 60px; }"
                       "#a { left: 10px; top: 10px; background-color: #e04020; } #b { left: 70px; top: 30px; background-color: #20a0e0; }"
                       "#c { left: 30px; top: 80px; background-color: #40e060; } #d { left: 120px; top: 90px; background-color: #f0e040; }"
                       "#g { position: absolute; left: 35px; top: 25px; width: 120px; height: 100px; border: 2px #ffffff80;"
                       " background-color: #ffffff20; backdrop-filter: blur(5px); }";
    const char* cells = "<div id=\"a\" class=\"c\"/><div id=\"b\" class=\"c\"/><div id=\"c\" class=\"c\"/><div id=\"d\" class=\"c\"/>";
    struct Case { const char* name; const char* style; const char* body; uint32_t fused; };
    const std::string glass = "<div id=\"g\">text</div>";
    const Case cases[] = {
        {"blur", "", "", 1},
        // its own rounded border box: the clip mask is drawn between the composites (into the
        // layer under the skipped one: they share the stencil)
        {"rounded", "#g { border-radius: 14px; }", "", 1},
        {"color and opacity", "#g { backdrop-filter: grayscale(0.8) brightness(1.2); opacity: 0.7; }", "", 1},
        {"with a filter", "#g { filter: drop-shadow(#000 3px 3px 2px); }", "", 1},
        {"two overlapping", "#g2 { position: absolute; left: 90px; top: 60px; width: 80px; height: 80px; backdrop-filter: blur(3px) sepia(1); }",
         "<div id=\"g2\"/>", 2},
        // under a rounded clipping ancestor (its clip mask is drawn between the composites too)
        {"rounded ancestor", "#clip { position: absolute; left: 20px; top: 20px; width: 150px; height: 120px; overflow: hidden; border-radius: 30px; }",
         "", 1},
    };
    for (const Case& c : cases){
        SCOPED_TRACE(c.name);
        const bool clipped = std::string(c.name) == "rounded ancestor";
        const std::string body = std::string(cells) + (clipped ? "<div id=\"clip\">" + glass + "</div>" : glass) + c.body;
        const std::string rml = std::string(head) + c.style + "</style></head><body>" + body + "</body></html>";
        RenderTestContext test(256, 192, 0, 0, 4);
        ASSERT_TRUE(test.isValid());
        ASSERT_NE(test.loadFromMemory(rml, "p1/backdrop-fused.html"), nullptr);
        RmlRenderer::setCompositeFusionForTesting(true);
        ASSERT_TRUE(test.renderUntilDrawn());
        const HtmlUIRenderStats fusedStats = test.renderer.getStats();
        std::vector<uint8_t> fused;
        ASSERT_TRUE(test.readLayer(fused));
        RmlRenderer::setCompositeFusionForTesting(false);
        const bool rendered = test.renderFrame();
        RmlRenderer::setCompositeFusionForTesting(true);
        ASSERT_TRUE(rendered);
        const HtmlUIRenderStats layeredStats = test.renderer.getStats();
        std::vector<uint8_t> layered;
        ASSERT_TRUE(test.readLayer(layered));
        EXPECT_EQ(fusedStats.compositesFused, c.fused);
        EXPECT_EQ(layeredStats.compositesFused, 0u);
        // each fused composite saves the temporary layer's pass
        EXPECT_EQ(fusedStats.passes + c.fused, layeredStats.passes);
        ASSERT_EQ(fused.size(), layered.size());
        size_t differing = 0;
        for (size_t i = 0; i < fused.size(); i++)
            differing += fused[i] != layered[i] ? 1 : 0;
        EXPECT_EQ(differing, 0u) << "the fused composite gives the temporary layer's image";
        // the blur reached the layer
        EXPECT_NE(std::count(fused.begin(), fused.end(), (uint8_t)0), (std::ptrdiff_t)fused.size());
    }
}

TEST(RenderP1Test, NestedClipStencilRefs){
    JASPER_REQUIRE_GPU();
    // (every mask in the stencil: no elision, no analytic clip)
    ClassicReplay classic;
    ASSERT_TRUE(HtmlUI::acquire());
    {
        RecordingClipRenderer renderer;
        RmlRenderTarget target;
        ASSERT_TRUE(target.ensure(512, 384, 4));
        Rml::Context* context = Rml::CreateContext("clip-refs", Rml::Vector2i(512, 384), &renderer);
        ASSERT_NE(context, nullptr);
        Rml::ElementDocument* document = context->LoadDocument("p1/clip_nested.rml");
        ASSERT_NE(document, nullptr);
        document->Show();

        // the pipelines the frame needs exist once it has been drawn
        bool drawn = false;
        for (int i = 0; i < 30 && !drawn; i++){
            beginTestFrame();
            drawn = renderer.beginFrame(target, Rml::Vector2i(512, 384));
            context->Update();
            if (drawn)
                context->Render();
            renderer.endFrame();
            SystemRender::commit();
        }
        ASSERT_TRUE(drawn);

        // from an empty cache, one frame: what it creates are exactly the frame's states
        SokolPipelineCache::clear();
        renderer.operations.clear();
        renderer.refs.clear();
        beginTestFrame();
        ASSERT_TRUE(renderer.beginFrame(target, Rml::Vector2i(512, 384)));
        context->Update();
        context->Render();
        ASSERT_TRUE(renderer.endFrame());
        SystemRender::commit();

        // RmlUi applies the whole list for each clipped element: [a], [a, b], [a, b, c]
        ASSERT_GE(renderer.refs.size(), 3u);
        const size_t n = renderer.refs.size();
        EXPECT_EQ(renderer.operations[n - 3], Rml::ClipMaskOperation::Set);
        EXPECT_EQ(renderer.operations[n - 2], Rml::ClipMaskOperation::Intersect);
        EXPECT_EQ(renderer.operations[n - 1], Rml::ClipMaskOperation::Intersect);
        EXPECT_EQ(renderer.refs[n - 3], 1);
        EXPECT_EQ(renderer.refs[n - 2], 2);
        EXPECT_EQ(renderer.refs[n - 1], 3);
        // geometry unclipped, stencil clear, Set write, Intersect write, geometry at refs 1, 2, 3
        EXPECT_EQ(SokolPipelineCache::size(), 7u);
        EXPECT_EQ(renderer.getStats().pipelineCacheMisses, 7u);
        EXPECT_EQ(renderer.getStats().passes, 1u);

        context->UnloadAllDocuments();
        context->Update();
        Rml::RemoveContext("clip-refs");
        Rml::ReleaseTextures(&renderer);
        Rml::ReleaseRenderManagers();
        renderer.releaseGpuResources();
    }
    HtmlUI::release();
}

TEST(RenderP1Test, SteadyStateNoAllocations){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    // filters, a backdrop filter, box shadows, a mask and gradients
    ASSERT_NE(test.load("p1/filter_chain.rml"), nullptr);
    ASSERT_NE(test.load("p1/box_shadow.rml"), nullptr);
    ASSERT_NE(test.load("p1/backdrop_filter.rml"), nullptr);
    ASSERT_NE(test.load("p1/mask_image.rml"), nullptr);
    ASSERT_NE(test.load("p1/gradients_linear.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    const uint64_t buffersAndTextures = test.renderer.getGpuCreateCount();
    const uint64_t framebuffers = test.target.getCreateCount();
    const size_t pipelines = SystemRender::getPipelineCacheSize();
    for (int frame = 0; frame < 10; frame++){
        ASSERT_TRUE(test.renderFrame());
        EXPECT_EQ(test.renderer.getStats().pipelineCacheMisses, 0u);
    }
    EXPECT_EQ(test.renderer.getGpuCreateCount(), buffersAndTextures);
    EXPECT_EQ(test.target.getCreateCount(), framebuffers);
    EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelines);
    EXPECT_GT(test.target.getLayerCount(), 1);
    EXPECT_GE(test.target.getPostprocessCount(), 3);
}

TEST(RenderP1Test, UnloadReleasesEverything){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    ASSERT_TRUE(test.renderUntilDrawn());
    const size_t initialTextures = test.renderer.getLiveTextureCount();

    size_t pipelinesAfterFirst = 0;
    for (int cycle = 0; cycle < 100; cycle++){
        Rml::ElementDocument* a = test.load("p1/box_shadow.rml");
        Rml::ElementDocument* b = test.load("p1/mask_image.rml");
        Rml::ElementDocument* c = test.load("p1/filter_drop_shadow.rml");
        ASSERT_TRUE(a && b && c);
        ASSERT_TRUE(test.renderFrame());
        EXPECT_GT(test.renderer.getStats().geometryLive, 0u);
        test.context->UnloadDocument(a);
        test.context->UnloadDocument(b);
        test.context->UnloadDocument(c);
        test.context->Update();
        // file textures stay in RmlUi's cache until released
        Rml::ReleaseTextures(&test.renderer);
        ASSERT_TRUE(test.renderFrame());

        EXPECT_EQ(test.renderer.getLiveGeometryCount(), 0u) << "cycle " << cycle;
        EXPECT_EQ(test.renderer.getStats().geometryLive, 0u);
        EXPECT_EQ(test.renderer.getStats().geometryBytes, 0u);
        EXPECT_EQ(test.renderer.getLiveTextureCount(), initialTextures) << "cycle " << cycle;
        if (cycle == 0)
            pipelinesAfterFirst = SystemRender::getPipelineCacheSize();
        else
            EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelinesAfterFirst);
        if (::testing::Test::HasFailure())
            break;
    }
    // storage is bounded: freed geometry storage is reused, not grown
    EXPECT_LT(test.renderer.getGeometryBufferCount(), 16u);
}

TEST(RenderP1Test, UnpremultipliedCopy){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(64, 64, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    const std::string rml = "<html><head><style>body { position: absolute; left: 0px; top: 0px; width: 64px; height: 64px; }"
                            " div { position: absolute; display: block; width: 32px; height: 32px; }"
                            " #red { left: 0px; top: 0px; background-color: #ff0000; }"
                            " #white { left: 32px; top: 32px; background-color: #ffffff80; }</style></head>"
                            "<body><div id=\"red\"/><div id=\"white\"/></body></html>";
    ASSERT_NE(test.loadFromMemory(rml, "p1/unpremultiply.html"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());

    FramebufferDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.depth = DepthStencilFormat::NONE;
    Framebuffer straight;
    ASSERT_TRUE(straight.create(desc));
    ASSERT_TRUE(test.renderer.drawUnpremultiplied(test.target, straight.getRender(), 64, 64));
    SystemRender::commit();

    std::vector<uint8_t> rgba;
    ASSERT_TRUE(readTexture(straight.getRender().getColorTexture(), 64, 64, rgba));
    PixelProbe probe{rgba, 64};
    // top-left origin: the red square at texel (0, 0)
    EXPECT_TRUE(probe.is(0, 0, 255, 0, 0, 255, 1));
    // 50 % white comes back straight
    EXPECT_TRUE(probe.is(48, 48, 255, 255, 255, 128, 1));
    // nothing: transparent
    EXPECT_TRUE(probe.is(48, 8, 0, 0, 0, 0, 0));
    straight.destroy();
}

TEST(RenderP1Test, TwoViewSizesKeepTheirTargets){
    JASPER_REQUIRE_GPU();
    SceneFixture fixture;
    fixture.addDocument("p1/filter_chain.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = *fixture.getScene().getSystem<UIDocumentSystem>();
    HtmlUIContext* context = system.getContext();
    ASSERT_NE(context, nullptr);

    // two views of different sizes draw the scene in turn (the editor's Scene and Game views)
    const Rect views[2] = {Rect(0, 0, 640, 360), Rect(0, 0, 800, 450)};
    RmlRenderTarget* targets[2] = {nullptr, nullptr};
    uint64_t creates[2] = {0, 0};
    for (int frame = 0; frame < 8; frame++){
        const int view = frame % 2;
        beginTestFrame();
        system.update(0);
        system.renderOffscreen(views[view]);
        ASSERT_TRUE(context->getRenderer().hasFrame(SystemRender::getFrameCount()));
        RmlRenderTarget* target = context->getRenderTarget();
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->getWidth(), (int)views[view].getWidth());
        // the context draws into the target of that size: from the third frame on, the same one
        // as two frames ago, never recreated
        if (frame >= 2){
            EXPECT_EQ(target, targets[view]);
            EXPECT_EQ(target->getCreateCount(), creates[view]);
        }
        targets[view] = target;
        creates[view] = target->getCreateCount();
        SystemRender::commit();
    }
    EXPECT_NE(targets[0], targets[1]);
    EXPECT_EQ(system.getRenderStats().passes, context->getRenderer().getStats().passes);
    EXPECT_GT(system.getRenderStats().passes, 1u);
    EXPECT_GT(system.getRenderStats().filtersApplied, 0u);
}
