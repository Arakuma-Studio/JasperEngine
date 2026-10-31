// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_RENDER_COMMON_H
#define HTMLUI_TEST_RENDER_COMMON_H

// RenderTestContext: an Rml::Context drawn by an RmlRenderer into its own RmlRenderTarget
// (layers with the given MSAA sample count), without scenes or the ECS. renderFrame() is the
// sequence of HtmlUIContext::render (beginFrame, Context::Update, Context::Render, endFrame),
// then the composite of layer 0 over an opaque #1E1E1E "screen" target, then
// SystemRender::commit(): one sokol frame per call.

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "RmlRenderer.h"

#include "Engine.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/SystemRender.h"
#include "shader/RuntimeShaderCompiler.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // S16 off for a scope (D1): every recorded call its own draw, meshes and shadow textures, every
    // clip mask in the stencil: what the phase-1 storage, leak and stencil tests count
    struct ClassicReplay {
        bool batching = RmlRenderer::isBatching();
        bool primitives = RmlRenderer::isSdfPrimitives();
        ClassicReplay(){
            RmlRenderer::setBatchingForTesting(false);
            RmlRenderer::setSdfPrimitivesForTesting(false);
        }
        ~ClassicReplay(){
            RmlRenderer::setBatchingForTesting(batching);
            RmlRenderer::setSdfPrimitivesForTesting(primitives);
        }
        ClassicReplay(const ClassicReplay&) = delete;
        ClassicReplay& operator=(const ClassicReplay&) = delete;
    };

    // S16 on for a scope, whatever the run's switches (JASPER_HTMLUI_S16_OFF=1, AC1): the tests of
    // S16's own features
    struct S16On {
        bool batching = RmlRenderer::isBatching();
        bool primitives = RmlRenderer::isSdfPrimitives();
        bool transformAware = RmlFontEngine::isTransformAwareText();
        S16On(){
            RmlRenderer::setBatchingForTesting(true);
            RmlRenderer::setSdfPrimitivesForTesting(true);
            RmlFontEngine::setTransformAwareTextForTesting(true);
        }
        ~S16On(){
            RmlRenderer::setBatchingForTesting(batching);
            RmlRenderer::setSdfPrimitivesForTesting(primitives);
            RmlFontEngine::setTransformAwareTextForTesting(transformAware);
        }
        S16On(const S16On&) = delete;
        S16On& operator=(const S16On&) = delete;
    };

    // what Engine::systemDraw does before a frame that the tests need: the shaders and pipelines
    // created off the render thread are published (spec S32), and the runtime shader compiler
    // compiles one waiting variant per frame on the main thread
    inline void beginTestFrame(){
        SystemRender::updateGpuCreation();
        RuntimeShaderCompiler::beginFrame();
    }

    class JASPERENGINE_INTERNAL RenderTestContext {
    public:
        static constexpr int background = 0x1E;

        // target (and screen) size in pixels, context dimensions in document px
        explicit RenderTestContext(int targetWidth = 640, int targetHeight = 360, int contextWidth = 0, int contextHeight = 0, int sampleCount = 1){
            targetSize = Rml::Vector2i(targetWidth, targetHeight);
            contextSize = Rml::Vector2i(contextWidth > 0 ? contextWidth : targetWidth, contextHeight > 0 ? contextHeight : targetHeight);

            if (!HtmlUI::acquire())
                return;
            acquired = true;

            static int counter = 0;
            name = "render-test-" + std::to_string(++counter);
            context = Rml::CreateContext(name, contextSize, &renderer);
            RmlRenderer::bindContext(context, &renderer);

            target.ensure(targetSize.x, targetSize.y, sampleCount);
            screen.createFramebuffer(TextureType::TEXTURE_2D, targetSize.x, targetSize.y, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                     TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false);
        }

        ~RenderTestContext(){
            destroy();
        }

        RenderTestContext(const RenderTestContext&) = delete;
        RenderTestContext& operator=(const RenderTestContext&) = delete;

        bool isValid() const{
            return context != nullptr;
        }

        // the order of HtmlUIContext::destroy: documents, the context (geometry), file
        // textures, render managers (font pages), then the GPU objects
        void destroy(){
            if (context){
                context->UnloadAllDocuments();
                context->Update();
                RmlRenderer::unbindContext(context);
                Rml::RemoveContext(name);
                context = nullptr;
                Rml::ReleaseTextures(&renderer);
                Rml::ReleaseRenderManagers();
            }
            renderer.releaseGpuResources();
            target.destroy();
            screen.destroyFramebuffer();
            if (acquired){
                HtmlUI::release();
                acquired = false;
            }
        }

        Rml::ElementDocument* load(const std::string& path){
            Rml::ElementDocument* document = context->LoadDocument(path);
            if (document)
                document->Show();
            return document;
        }

        Rml::ElementDocument* loadFromMemory(const std::string& rml, const std::string& sourceUrl){
            Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, sourceUrl);
            if (document)
                document->Show();
            return document;
        }

        // one frame; false when the renderer dropped it (shaders not ready). The target is
        // created again after a view loss, as HtmlUIContext::render does
        bool renderFrame(){
            beginTestFrame();
            if (!target.isCreated())
                target.ensure(targetSize.x, targetSize.y, sampleCount());
            const bool begun = renderer.beginFrame(target, contextSize);
            // as HtmlUIContext: every edge of backgrounds and borders feathered without
            // multisampling, curves only with it, one target pixel wide
            Rml::SetGeometryFeathering(sampleCount() <= 1);
            Rml::SetGeometryPixelSize(targetSize.x > 0 ? (float)contextSize.x / (float)targetSize.x : 1.0f);
            if (context){
                context->Update();
                // <video> frames chosen in the update (HtmlUIContext converts them before beginFrame)
                renderer.runVideoConversions();
                if (begun){
                    // the font engine's counters of this context's frame, as HtmlUIContext::render
                    RmlFontEngine& fonts = HtmlUI::getFontEngine();
                    fonts.beginContextFrame(context->GetRenderManager());
                    context->Render();
                    const RmlFontEngine::FrameCounters text = fonts.takeContextFrame(context->GetRenderManager());
                    renderer.addTextStats(text.glyphsRasterized, text.textRegenerations, text.textEffectFallbacks, text.atlasEvictions);
                }
                // text settling at its on-screen scale asks for another frame (S16 R15)
                renderRequested = context->GetRenderManager().ConsumeRenderRequest();
            }
            bool drawn = renderer.endFrame();
            if (drawn)
                compositeToScreen();
            SystemRender::commit();
            return drawn;
        }

        int sampleCount() const{
            return target.getSampleCount();
        }

        // frames until one is drawn (the UI shader variants compile, one per frame, on the
        // first ones), then one more so anything loaded during the first frame is in place
        bool renderUntilDrawn(int maxFrames = 30){
            for (int i = 0; i < maxFrames; i++){
                if (renderFrame())
                    return renderFrame();
            }
            return false;
        }

        // the document layer (layer 0, resolved): premultiplied RGBA, transparent where
        // nothing was drawn
        bool readLayer(std::vector<uint8_t>& rgba){
            FramebufferRender* layer = target.getComposited();
            return layer && readTexture(layer->getColorTexture(), targetSize.x, targetSize.y, rgba);
        }

        // the layer composited over the background
        bool readScreen(std::vector<uint8_t>& rgba){
            return readTexture(screen.getColorTexture(), targetSize.x, targetSize.y, rgba);
        }

        CompareResult compareScreen(const std::string& goldenName, const CompareOptions& options = {}){
            std::vector<uint8_t> rgba;
            if (!readScreen(rgba)){
                CompareResult result;
                result.message = "readback of the screen target failed";
                return result;
            }
            return compareWithGolden(goldenName, rgba, targetSize.x, targetSize.y, options);
        }

        Rml::Context* context = nullptr;
        // the last frame asked for another one (HtmlUIContext keeps updatedSinceRender then)
        bool renderRequested = false;
        RmlRenderer renderer;
        RmlRenderTarget target;
        FramebufferRender screen;
        Rml::Vector2i contextSize;
        Rml::Vector2i targetSize;

    private:
        void compositeToScreen(){
            const float bg = background / 255.0f;
            screenPass.setClearColor(Vector4(bg, bg, bg, 1.0f));
            screenPass.startRenderPass(&screen);
            const Rect full(0, 0, (float)targetSize.x, (float)targetSize.y);
            screenPass.applyViewport(full);
            screenPass.applyScissor(full);
            // an offscreen camera pass renders Y-flipped on GL (RenderSystem::isRenderingFlipped)
            renderer.drawComposite(target.getComposited()->getColorTexture(), PIP_RTT_NODEPTH, Engine::isOpenGL());
            screenPass.endRenderPass();
        }

        std::string name;
        bool acquired = false;
        CameraRender screenPass;
    };

    // RGBA pixel probes on a top-row-first image
    struct PixelProbe {
        const std::vector<uint8_t>& rgba;
        int width;

        const uint8_t* at(int x, int y) const{
            return &rgba[(size_t(y) * width + x) * 4];
        }

        ::testing::AssertionResult is(int x, int y, int r, int g, int b, int a = 255, int tolerance = 2) const{
            const uint8_t* p = at(x, y);
            if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance && std::abs(p[3] - a) <= tolerance)
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "pixel (" << x << ", " << y << ") is (" << int(p[0]) << ", " << int(p[1]) << ", "
                                                 << int(p[2]) << ", " << int(p[3]) << "), expected (" << r << ", " << g << ", " << b
                                                 << ", " << a << ") +-" << tolerance;
        }

        ::testing::AssertionResult isBackground(int x, int y) const{
            return is(x, y, RenderTestContext::background, RenderTestContext::background, RenderTestContext::background);
        }
    };

} }

#endif //HTMLUI_TEST_RENDER_COMMON_H
