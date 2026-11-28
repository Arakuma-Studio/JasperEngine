// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The render half of HtmlUIContext (HtmlUIContext.h): the context draws its documents into
// its own offscreen RmlRenderTarget before any camera pass (layers, filters and stencil clips
// included), only when the recorded frame differs from the one the target holds (S92 R1), and
// the camera pass composites the tiles of layer 0 the frame wrote (S92 R4).

#include "HtmlUIContext.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "RmlRenderer.h"

#include "Engine.h"
#include "Log.h"
#include "render/FramebufferRender.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    bool hasVisibleDocument(Rml::Context* context){
        for (int i = 0; i < context->GetNumDocuments(); i++){
            Rml::ElementDocument* document = context->GetDocument(i);
            if (document && document->IsVisible())
                return true;
        }
        return false;
    }

    // the project's count; 0 = Auto (autoSampleCount of the running platform)
    int preferredSampleCount = 4;

    // 1, 2, 4 or 8 (another count goes to the one below it, at least 1); 0 stays 0
    int normalizedSampleCount(int samples){
        if (samples <= 0)
            return 0;
        return samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1;
    }

}

void HtmlUIContext::setPreferredSampleCount(int samples){
    const int valid = normalizedSampleCount(samples);
    if (valid != samples)
        Log::warn("[HTML UI] MSAA sample count %d is not 0 (Auto), 1, 2, 4 or 8: %d is used", samples, valid);
    preferredSampleCount = valid;
}

int HtmlUIContext::getPreferredSampleCount(){
    return preferredSampleCount;
}

int HtmlUIContext::autoSampleCount(Platform platform){
    // phones and the web: one sample (SDF primitives and analytic clips antialias at any count,
    // and a 2.6 MP target is a quarter of the memory and bandwidth); desktop: 4
    switch (platform){
        case Platform::Android:
        case Platform::iOS:
        case Platform::Web:
            return 1;
        case Platform::Windows:
        case Platform::Linux:
        case Platform::MacOS:
            return 4;
    }
    return 4;
}

void HtmlUIContext::setSampleCountOverride(int samples){
    sampleCountOverride = normalizedSampleCount(samples);
}

int HtmlUIContext::getSampleCountOverride() const{
    return sampleCountOverride;
}

int HtmlUIContext::getRequestedSampleCount() const{
    if (sampleCountOverride > 0)
        return sampleCountOverride;
    return preferredSampleCount > 0 ? preferredSampleCount : autoSampleCount(Engine::getPlatform());
}

int HtmlUIContext::getSampleCount() const{
    const int requested = getRequestedSampleCount();
    if (requested <= 1)
        return 1;
    // the first sg_query_pixelformat calls of the engine, cached
    const RenderFeatures& features = SystemRender::getFeatures();
    if (!features.msaaRGBA8 || !features.msaaDepthStencil)
        return 1;
    // the highest count up to it that the device renders (RmlRenderTarget::ensure)
    return std::max(FramebufferRender::getMaxMsaaSamples(requested), 1);
}

void HtmlUIContext::requestRedraw(){
    if (renderer)
        renderer->addLiveSource(RmlRenderer::LIVE_REQUESTED);
}

bool HtmlUIContext::render(const Rect& viewRect){
    if (!renderer)
        return false;

    // once per sokol frame: a second call (the editor's Game view after the frame's own
    // passes) keeps the first one's result
    const uint64_t frame = SystemRender::getFrameCount();
    if (lastRenderFrame == frame)
        return hasContent;
    lastRenderFrame = frame;

    if (!context){
        hasContent = false;
        return false;
    }

    // textures RmlUi made before the view existed have no GPU texture: it makes them again
    // while it draws (as after a view loss)
    if (renderer->takeTexturesWithoutView()){
        Rml::ReleaseTextures(renderer.get());
        renderer->addLiveSource(RmlRenderer::LIVE_REQUESTED);
    }

    const Rml::Vector2i size((int)std::lround(viewRect.getWidth()), (int)std::lround(viewRect.getHeight()));
    const Rml::Vector2i contextDimensions = context->GetDimensions();
    if (size.x <= 0 || size.y <= 0 || contextDimensions.x <= 0 || contextDimensions.y <= 0 || !hasVisibleDocument(context)){
        hasContent = false;
        return false;
    }

    // view pixels per document pixel; glyphs rasterize at it, so text stays crisp on a scaled
    // canvas (a change regenerates the text): exactly with transform-aware text (S16 R14), else
    // quantized to quarters within [1, 4] as before
    pixelRatio = std::min(std::max(viewRect.getWidth() / (float)contextDimensions.x, 0.25f), 8.0f);
    RmlFontEngine& fontEngine = HtmlUI::getFontEngine();
    const float rasterScale = RmlFontEngine::isTransformAwareText() ? pixelRatio
                                                                    : std::min(std::max(std::round(pixelRatio * 4.0f) / 4.0f, 1.0f), 4.0f);
    fontEngine.setRasterScale(context->GetRenderManager(), rasterScale);

    // recreated when the view size or the sample count changes; a second size (another view)
    // keeps its own target
    const int samples = getSampleCount();
    // the fork feathers every edge of backgrounds and borders on a single-sampled target, only
    // their curves on a multisampled one (phase-3 spec 3.7.3)
    Rml::SetGeometryFeathering(samples <= 1);
    Rml::SetGeometryPixelSize(1.0f / pixelRatio);
    auto matches = [&](const std::unique_ptr<RmlRenderTarget>& t){
        return t && t->isCreated() && t->getWidth() == size.x && t->getHeight() == size.y && t->getSampleCount() == samples;
    };
    if (!matches(renderTarget)){
        if (!spareTarget)
            spareTarget.reset(new RmlRenderTarget());
        std::swap(renderTarget, spareTarget);
        hasContent = false;
        if (spareTarget->isCreated() && spareTarget->getSampleCount() != samples)
            spareTarget->destroy();
    }
    if (renderTarget->ensure(size.x, size.y, samples) || !renderTarget->isCreated()){
        hasContent = false;
        if (!renderTarget->isCreated()){
            Log::error("[HTML UI] could not create the %d x %d document target of '%s'", size.x, size.y, name.c_str());
            renderTarget->destroy();
            return false;
        }
    }

    // <video> frames decoded since the last frame, converted before the document draws them
    renderer->runVideoConversions();
    if (!renderer->beginFrame(*renderTarget, contextDimensions)){
        // shaders still compiling: nothing drawn, the old content (if any) stays valid
        renderer->endFrame();
        return hasContent;
    }
    Rml::RenderManager& renderManager = context->GetRenderManager();
    fontEngine.beginContextFrame(renderManager);
    context->Render();
    const RmlFontEngine::FrameCounters text = fontEngine.takeContextFrame(renderManager);
    renderer->addTextStats(text.glyphsRasterized, text.textRegenerations, text.textEffectFallbacks, text.atlasEvictions);
    if (renderer->endFrame()){
        hasContent = true;
        renderedOnce = true;
    }
    // text settling at its on-screen scale (S16 R15) draws again next frame, without input
    if (renderManager.ConsumeRenderRequest())
        renderer->addLiveSource(RmlRenderer::LIVE_TEXT_SETTLE);

    return hasContent;
}

void HtmlUIContext::composite(PipelineType colorPipeline, bool flipY){
    composite(colorPipeline, flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity());
}

void HtmlUIContext::composite(PipelineType colorPipeline, const Rml::Matrix4f& clipFromQuad, int msaaSamples){
    FramebufferRender* layer = renderTarget ? renderTarget->getComposited() : nullptr;
    if (!hasContent || !renderer || !layer)
        return;

    // the camera pass's target, without depth: documents are in front of everything
    PipelineType compositePipeline = PIP_RTT_NODEPTH;
    if (colorPipeline == PIP_DEFAULT || colorPipeline == PIP_DEFAULT_NODEPTH)
        compositePipeline = PIP_DEFAULT_NODEPTH;
    else if (colorPipeline == PIP_MSAA || colorPipeline == PIP_MSAA_NODEPTH)
        compositePipeline = PIP_MSAA_NODEPTH;

    renderer->drawComposite(layer->getColorTexture(), compositePipeline, clipFromQuad, msaaSamples);
}

void HtmlUIContext::releaseGpuResources(){
    hasContent = false;
    renderedOnce = false;
    lastRenderFrame = UINT64_MAX;

    // RmlUi gives its textures back (and loads them again when it draws next), so no
    // handle points at a texture that went with the view
    if (renderer && context && HtmlUI::isInitialized()){
        Rml::ReleaseTextures(renderer.get());
    }
    if (renderer){
        renderer->releaseGpuResources();
    }

    if (renderTarget)
        renderTarget->destroy();
    if (spareTarget)
        spareTarget->destroy();
}

bool HtmlUIContext::hasRenderedFrame() const{
    return renderedOnce;
}

float HtmlUIContext::getPixelRatio() const{
    return pixelRatio;
}

FramebufferRender* HtmlUIContext::getTarget(){
    return renderTarget ? renderTarget->getComposited() : nullptr;
}

RmlRenderTarget* HtmlUIContext::getRenderTarget(){
    return renderTarget.get();
}

RmlRenderer& HtmlUIContext::getRenderer(){
    return *renderer;
}
