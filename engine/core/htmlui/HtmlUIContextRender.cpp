// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The render half of HtmlUIContext (HtmlUIContext.h): the context draws its documents into
// its own offscreen target before any camera pass, and the camera pass composites that
// target as one quad.

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

}

bool HtmlUIContext::render(const Rect& viewRect){
    if (!renderer){
        renderedThisFrame = false;
        return false;
    }

    // once per sokol frame: a second call (the editor's Game view after the frame's own
    // passes) keeps the first one's result, the stream buffers take one update a frame
    if (renderer->hasFrame(SystemRender::getFrameCount()))
        return renderedThisFrame;

    renderedThisFrame = false;

    if (!context)
        return false;

    const Rml::Vector2i size((int)std::lround(viewRect.getWidth()), (int)std::lround(viewRect.getHeight()));
    const Rml::Vector2i contextDimensions = context->GetDimensions();
    if (size.x <= 0 || size.y <= 0 || contextDimensions.x <= 0 || contextDimensions.y <= 0 || !hasVisibleDocument(context))
        return false;

    // view pixels per document pixel; glyphs rasterize at it (quantized), so text stays
    // crisp on a scaled canvas (a change regenerates the text)
    pixelRatio = std::min(std::max(viewRect.getWidth() / (float)contextDimensions.x, 0.25f), 8.0f);
    const float rasterScale = std::min(std::max(std::round(pixelRatio * 4.0f) / 4.0f, 1.0f), 4.0f);
    HtmlUI::getFontEngine().setRasterScale(rasterScale);

    if (size != targetSize || !target.isCreated()){
        target.destroyFramebuffer();
        if (!target.createFramebuffer(TextureType::TEXTURE_2D, size.x, size.y, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                      TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false)){
            Log::error("[HTML UI] could not create the %d x %d document target of '%s'", size.x, size.y, name.c_str());
            targetSize = Rml::Vector2i(0, 0);
            return false;
        }
        targetSize = size;
    }

    renderer->beginFrame(contextDimensions, targetSize);
    context->Render();
    renderedThisFrame = renderer->endFrame(target, pass);
    if (renderedThisFrame)
        renderedOnce = true;

    return renderedThisFrame;
}

void HtmlUIContext::composite(PipelineType colorPipeline, bool flipY){
    if (!renderedThisFrame || !renderer || !target.isCreated())
        return;

    // the camera pass's target, without depth: documents are in front of everything
    PipelineType compositePipeline = PIP_RTT_NODEPTH;
    if (colorPipeline == PIP_DEFAULT || colorPipeline == PIP_DEFAULT_NODEPTH)
        compositePipeline = PIP_DEFAULT_NODEPTH;

    renderer->drawComposite(target.getColorTexture(), compositePipeline, flipY);
}

void HtmlUIContext::releaseGpuResources(){
    renderedThisFrame = false;
    renderedOnce = false;

    // RmlUi gives its textures back (and loads them again when it draws next), so no
    // handle points at a texture that went with the view
    if (renderer && context && HtmlUI::isInitialized()){
        Rml::ReleaseTextures(renderer.get());
    }
    if (renderer){
        renderer->releaseGpuResources();
    }

    target.destroyFramebuffer();
    targetSize = Rml::Vector2i(0, 0);
}

bool HtmlUIContext::hasRenderedFrame() const{
    return renderedOnce;
}

float HtmlUIContext::getPixelRatio() const{
    return pixelRatio;
}

FramebufferRender* HtmlUIContext::getTarget(){
    return target.isCreated() ? &target : nullptr;
}

RmlRenderer& HtmlUIContext::getRenderer(){
    return *renderer;
}
