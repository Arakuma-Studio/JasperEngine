// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The render half of HtmlUIContext (HtmlUIContext.h)

#include "HtmlUIContext.h"

#include "RmlRenderer.h"

using namespace jasperengine;

bool HtmlUIContext::render(const Rect& viewRect){
    // HTMLUI_SKELETON: p0-render
    (void)viewRect;
    return false;
}

void HtmlUIContext::composite(PipelineType colorPipeline, bool flipY){
    // HTMLUI_SKELETON: p0-render
    (void)colorPipeline;
    (void)flipY;
}

void HtmlUIContext::releaseGpuResources(){
    // HTMLUI_SKELETON: p0-render
}

bool HtmlUIContext::hasRenderedFrame() const{
    // HTMLUI_SKELETON: p0-render
    return false;
}

float HtmlUIContext::getPixelRatio() const{
    // HTMLUI_SKELETON: p0-render
    return 1.0f;
}

FramebufferRender* HtmlUIContext::getTarget(){
    // HTMLUI_SKELETON: p0-render
    return nullptr;
}

RmlRenderer& HtmlUIContext::getRenderer(){
    // HTMLUI_SKELETON: p0-render
    return *renderer;
}
