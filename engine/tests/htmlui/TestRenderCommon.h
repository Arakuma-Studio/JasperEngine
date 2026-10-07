// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_RENDER_COMMON_H
#define HTMLUI_TEST_RENDER_COMMON_H

// RenderTestContext: an Rml::Context drawn by an RmlRenderer into its own target, without
// scenes or the ECS. renderFrame() is the sequence of HtmlUIContext::render (beginFrame,
// Context::Update, Context::Render, endFrame), then the composite of that layer over an
// opaque #1E1E1E "screen" target, then SystemRender::commit(): one sokol frame per call.

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
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

    // what Engine::systemDraw does before a frame that the tests need: the runtime shader
    // compiler compiles one waiting variant per frame on the main thread
    inline void beginTestFrame(){
        RuntimeShaderCompiler::beginFrame();
    }

    class JASPERENGINE_INTERNAL RenderTestContext {
    public:
        static constexpr int background = 0x1E;

        // target (and screen) size in pixels, context dimensions in document px
        explicit RenderTestContext(int targetWidth = 640, int targetHeight = 360, int contextWidth = 0, int contextHeight = 0){
            targetSize = Rml::Vector2i(targetWidth, targetHeight);
            contextSize = Rml::Vector2i(contextWidth > 0 ? contextWidth : targetWidth, contextHeight > 0 ? contextHeight : targetHeight);

            if (!HtmlUI::acquire())
                return;
            acquired = true;

            static int counter = 0;
            name = "render-test-" + std::to_string(++counter);
            context = Rml::CreateContext(name, contextSize, &renderer);

            target.createFramebuffer(TextureType::TEXTURE_2D, targetSize.x, targetSize.y, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                     TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false);
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
                Rml::RemoveContext(name);
                context = nullptr;
                Rml::ReleaseTextures(&renderer);
                Rml::ReleaseRenderManagers();
            }
            renderer.releaseGpuResources();
            target.destroyFramebuffer();
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

        // one frame; false when the renderer dropped it (shaders not ready)
        bool renderFrame(){
            beginTestFrame();
            renderer.beginFrame(contextSize, targetSize);
            if (context){
                context->Update();
                context->Render();
            }
            bool drawn = renderer.endFrame(target, layerPass);
            if (drawn)
                compositeToScreen();
            SystemRender::commit();
            return drawn;
        }

        // frames until one is drawn (shaders compile on the first), then one more so
        // anything loaded during the first frame is in place
        bool renderUntilDrawn(int maxFrames = 10){
            for (int i = 0; i < maxFrames; i++){
                if (renderFrame())
                    return renderFrame();
            }
            return false;
        }

        // the document layer: premultiplied RGBA, transparent where nothing was drawn
        bool readLayer(std::vector<uint8_t>& rgba){
            return readTexture(target.getColorTexture(), targetSize.x, targetSize.y, rgba);
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
        RmlRenderer renderer;
        FramebufferRender target;
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
            renderer.drawComposite(target.getColorTexture(), PIP_RTT_NODEPTH, Engine::isOpenGL());
            screenPass.endRenderPass();
        }

        std::string name;
        bool acquired = false;
        CameraRender layerPass;
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
