// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem rendering (phase 1, p1-render, spec §3.5): HtmlUIRenderState, the
// offscreen passes of the scene's contexts, the camera-aware composite, render statistics and
// the MSAA sample count.

#include "UIDocumentSystem.h"

#include "Engine.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "htmlui/HtmlUI.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/RmlFontEngine.h"
#include "htmlui/RmlRenderer.h"
#include "math/Matrix4.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cstring>

namespace jasperengine{

    struct HtmlUIRenderState {
        HtmlUIRenderStats stats;   // the last renderOffscreen, summed over the scene's contexts

        // a context's part: the frame counters when it rendered in this sokol frame, the
        // geometry counts always
        void add(HtmlUIContext* context){
            if (!context)
                return;
            RmlRenderer& renderer = context->getRenderer();
            stats.add(renderer.getStats(), renderer.hasFrame(SystemRender::getFrameCount()));
        }
    };

}

using namespace jasperengine;

void UIDocumentSystem::setPreferredSampleCount(int samples){
    HtmlUIContext::setPreferredSampleCount(samples);
}

int UIDocumentSystem::getPreferredSampleCount(){
    return HtmlUIContext::getPreferredSampleCount();
}

void UIDocumentSystem::setHtmlUiRenderOptions(int glyphAtlasMiB, bool sdfPrimitives, bool transformAwareText){
    RmlFontEngine::setAtlasBudgetMiB((size_t)std::max(glyphAtlasMiB, 0));
    const bool primitivesChanged = RmlRenderer::isSdfPrimitives() != sdfPrimitives;
    RmlRenderer::setSdfPrimitivesForTesting(sdfPrimitives);
    RmlFontEngine::setTransformAwareTextForTesting(transformAwareText);
    for (HtmlUIContext* context : HtmlUI::getContexts()){
        if (primitivesChanged && context->getRmlContext())
            context->getRmlContext()->DirtyBackgrounds();
        context->markUpdated();
    }
}

void UIDocumentSystem::renderOffscreen(const Rect& viewRect){
    if (!renderState)
        renderState = std::make_shared<HtmlUIRenderState>();
    renderState->stats = HtmlUIRenderStats();

    // the screen context at the view's size (texture-mode contexts render at their own, p1-rtt)
    HtmlUIContext* screen = getContext();
    if (screen){
        screen->render(viewRect);
        renderState->add(screen);
    }
    textureRenderOffscreen(); for (HtmlUIContext* texture : getTextureContexts()) renderState->add(texture);
}

const HtmlUIRenderStats& UIDocumentSystem::getRenderStats() const{
    static const HtmlUIRenderStats none;
    return renderState ? renderState->stats : none;
}

HtmlUIRenderStats UIDocumentSystem::getRenderStats(Entity entity) const{
    HtmlUIContext* context = getTextureContext(entity);
    if (!context)
        context = getContext();
    return context ? context->getRenderer().getStats() : HtmlUIRenderStats();
}

void UIDocumentSystem::drawComposite(PipelineType colorPipeline, bool flipY){
    HtmlUIContext* screen = getContext();
    if (screen)
        screen->composite(colorPipeline, flipY);
}

void UIDocumentSystem::drawComposite(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPipeline, bool flipY,
                                     int msaaSamples){
    (void)cameraTransform;
    HtmlUIContext* screen = getContext();
    if (!screen)
        return;

    // a 3D camera has no canvas: the document fills the viewport
    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        screen->composite(colorPipeline, flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity(), msaaSamples);
        return;
    }

    // UI and 2D cameras: the canvas rectangle (0,0)-(width,height) of the world space where
    // RectTransform UI lives, through the camera, so a panned or zoomed view (the editor's
    // Scene view) shows the document in place. The document's top edge is at y = 0 under a
    // UI camera (y down) and at y = height under an orthographic one (y up, UI flipped)
    float w = 0.0f, h = 0.0f;
    getFrameSize(w, h);
    if (w <= 0.0f || h <= 0.0f)
        return;

    const float ySign = (camera.type == CameraType::CAMERA_UI) ? -1.0f : 1.0f;
    // quad corners (-1,1) top-left .. (1,-1) bottom-right to world
    const Matrix4 worldFromQuad(0.5f * w, 0.0f, 0.0f, 0.5f * w,
                                0.0f, 0.5f * h * ySign, 0.0f, 0.5f * h,
                                0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1.0f);
    Matrix4 clipFromQuad = camera.viewProjectionMatrix * worldFromQuad;
    if (flipY)
        clipFromQuad = Matrix4::scaleMatrix(Vector3(1.0f, -1.0f, 1.0f)) * clipFromQuad;

    // both column-major
    Rml::Matrix4f matrix;
    std::memcpy(matrix.data(), (const float*)clipFromQuad, sizeof(float) * 16);
    screen->composite(colorPipeline, matrix, msaaSamples);
}
