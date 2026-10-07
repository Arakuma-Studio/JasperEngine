// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The render half of HtmlUIContext (HtmlUIContext.h): the context draws its documents into
// its own offscreen RmlRenderTarget before any camera pass (layers, filters and stencil clips
// included), and the camera pass composites layer 0 as one quad.

#include "HtmlUIContext.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "RmlRenderer.h"

#include "Log.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

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

    int preferredSampleCount = 4;

}

void HtmlUIContext::setPreferredSampleCount(int samples){
    // 1, 2, 4 or 8: anything else goes to the nearest of them
    int clamped = 1;
    if (samples >= 8)
        clamped = 8;
    else if (samples >= 4)
        clamped = 4;
    else if (samples >= 2)
        clamped = 2;
    if (clamped != samples)
        Log::warn("[HTML UI] MSAA sample count %d is not 1, 2, 4 or 8: %d is used", samples, clamped);
    preferredSampleCount = clamped;
}

int HtmlUIContext::getPreferredSampleCount(){
    return preferredSampleCount;
}

int HtmlUIContext::getSampleCount() const{
    if (preferredSampleCount <= 1)
        return 1;
    // the first sg_query_pixelformat calls of the engine, cached
    const RenderFeatures& features = SystemRender::getFeatures();
    if (!features.msaaRGBA8 || !features.msaaDepthStencil)
        return 1;
    return preferredSampleCount;
}

void HtmlUIContext::markUpdated(){
    updatedSinceRender = true;
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

    const Rml::Vector2i size((int)std::lround(viewRect.getWidth()), (int)std::lround(viewRect.getHeight()));
    const Rml::Vector2i contextDimensions = context->GetDimensions();
    if (size.x <= 0 || size.y <= 0 || contextDimensions.x <= 0 || contextDimensions.y <= 0 || !hasVisibleDocument(context)){
        hasContent = false;
        return false;
    }

    // view pixels per document pixel; glyphs rasterize at it (quantized), so text stays
    // crisp on a scaled canvas (a change regenerates the text)
    pixelRatio = std::min(std::max(viewRect.getWidth() / (float)contextDimensions.x, 0.25f), 8.0f);
    const float rasterScale = std::min(std::max(std::round(pixelRatio * 4.0f) / 4.0f, 1.0f), 4.0f);
    HtmlUI::getFontEngine().setRasterScale(rasterScale);

    // recreated when the view size or the sample count changes
    if (renderTarget->ensure(size.x, size.y, getSampleCount()) || !renderTarget->isCreated()){
        hasContent = false;
        if (!renderTarget->isCreated()){
            Log::error("[HTML UI] could not create the %d x %d document target of '%s'", size.x, size.y, name.c_str());
            renderTarget->destroy();
            return false;
        }
    }

    // nothing changed since the last frame: the target still holds it
    if (hasContent && !updatedSinceRender && contextDimensions == targetDimensions)
        return true;

    if (!renderer->beginFrame(*renderTarget, contextDimensions)){
        // shaders still compiling: nothing drawn, the old content (if any) stays valid
        renderer->endFrame();
        return hasContent;
    }
    context->Render();
    if (renderer->endFrame()){
        hasContent = true;
        renderedOnce = true;
        updatedSinceRender = false;
        targetDimensions = contextDimensions;
    }

    return hasContent;
}

void HtmlUIContext::composite(PipelineType colorPipeline, bool flipY){
    composite(colorPipeline, flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity());
}

void HtmlUIContext::composite(PipelineType colorPipeline, const Rml::Matrix4f& clipFromQuad){
    FramebufferRender* layer = renderTarget ? renderTarget->getComposited() : nullptr;
    if (!hasContent || !renderer || !layer)
        return;

    // the camera pass's target, without depth: documents are in front of everything
    PipelineType compositePipeline = PIP_RTT_NODEPTH;
    if (colorPipeline == PIP_DEFAULT || colorPipeline == PIP_DEFAULT_NODEPTH)
        compositePipeline = PIP_DEFAULT_NODEPTH;

    renderer->drawComposite(layer->getColorTexture(), compositePipeline, clipFromQuad);
}

void HtmlUIContext::releaseGpuResources(){
    hasContent = false;
    renderedOnce = false;
    updatedSinceRender = true;
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
