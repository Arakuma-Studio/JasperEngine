// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem rendering (phase 1, p1-render, spec §3.5): HtmlUIRenderState, the
// offscreen passes of the scene's contexts, the camera-aware composite, render statistics and
// the MSAA sample count.

#include "UIDocumentSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/Transform.h"
#include "htmlui/HtmlUI.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/RmlFontEngine.h"
#include "htmlui/RmlRenderer.h"
#include "math/Matrix4.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace jasperengine{

    // S92 R32: a context's renderer statistics with the target it holds: its sample count, and
    // the bytes of its images (each layer's colour and depth-stencil at every sample, 4 + 4 bytes a
    // pixel, and its resolve image under MSAA; the single-sampled post-process targets; the region
    // layers and their sets)
    HtmlUIRenderStats contextStats(HtmlUIContext& context){
        HtmlUIRenderStats stats = context.getRenderer().getStats();
        const RmlRenderTarget* target = context.getRenderTarget();
        if (!target || !target->isCreated())
            return stats;
        const uint64_t pixels = (uint64_t)target->getWidth() * (uint64_t)target->getHeight();
        const uint64_t samples = (uint64_t)std::max(target->getSampleCount(), 1);
        const uint64_t layer = pixels * 8 * samples + (samples > 1 ? pixels * 4 : 0);
        const uint64_t bytes = layer * (uint64_t)target->getLayerCount() + pixels * 4 * (uint64_t)target->getPostprocessCount()
                             + (uint64_t)target->getRegionBytes();
        stats.samples = (uint32_t)samples;
        stats.targetBytes = (uint32_t)std::min<uint64_t>(bytes, UINT32_MAX);
        return stats;
    }

    // the screen documents' quad in the viewport: all of it, or, in a view showing another canvas
    // than the documents' frame (the editor's Game view outside Play lays them out in the
    // project's canvas), the frame's part of the view's canvas from its top-left corner: canvas
    // pixels keep their size and the view crops the rest, as the game shows them
    Rml::Matrix4f screenQuadMatrix(bool flipY, float frameWidth, float frameHeight, float viewCanvasWidth, float viewCanvasHeight){
        Rml::Matrix4f matrix = Rml::Matrix4f::Identity();
        if (frameWidth > 0.0f && frameHeight > 0.0f && viewCanvasWidth > 0.0f && viewCanvasHeight > 0.0f){
            const float sx = frameWidth / viewCanvasWidth, sy = frameHeight / viewCanvasHeight;
            matrix = Rml::Matrix4f::Translate(sx - 1.0f, 1.0f - sy, 0.0f) * Rml::Matrix4f::Scale(sx, sy, 1.0f);
        }
        return flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) * matrix : matrix;
    }

    struct HtmlUIRenderState {
        HtmlUIRenderStats stats;   // the last renderOffscreen, summed over the scene's contexts

        // a context's part: the frame counters when it rendered in this sokol frame, the
        // geometry counts and its target always
        void add(HtmlUIContext* context){
            if (!context)
                return;
            stats.add(contextStats(*context), context->getRenderer().hasFrame(SystemRender::getFrameCount()));
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
        context->requestRedraw();
    }
}

void UIDocumentSystem::renderOffscreen(const Rect& viewRect){
    if (!renderState)
        renderState = std::make_shared<HtmlUIRenderState>();
    renderState->stats = HtmlUIRenderStats();

    // S92 R17-R19: the scene's MSAA count for its screen context; a texture-mode document's own,
    // else the scene's, for its context (0: the project's). A change recreates the target
    const int sceneSamples = scene->getHtmlUiMsaaSamples();
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        const UIDocumentComponent& component = components->getComponentFromIndex(i);
        if (component.renderMode != UIDocumentRenderMode::TEXTURE)
            continue;
        if (HtmlUIContext* texture = getTextureContext(components->getEntity(i)))
            texture->setSampleCountOverride(component.msaaSamples > 0 ? component.msaaSamples : sceneSamples);
    }

    // the screen context at the view's size (texture-mode contexts render at their own, p1-rtt)
    HtmlUIContext* screen = getContext();
    if (screen){
        screen->setSampleCountOverride(sceneSamples);
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
    return context ? contextStats(*context) : HtmlUIRenderStats();
}

void UIDocumentSystem::drawComposite(PipelineType colorPipeline, bool flipY, int msaaSamples, float viewCanvasWidth, float viewCanvasHeight){
    HtmlUIContext* screen = getContext();
    if (!screen)
        return;
    float w = 0.0f, h = 0.0f;
    if (viewCanvasWidth > 0.0f && viewCanvasHeight > 0.0f)
        getFrameSize(w, h);
    screen->composite(colorPipeline, screenQuadMatrix(flipY, w, h, viewCanvasWidth, viewCanvasHeight), msaaSamples);
}

void UIDocumentSystem::drawComposite(CameraComponent& camera, Transform& cameraTransform, PipelineType colorPipeline, bool flipY,
                                     int msaaSamples, bool throughCamera, float viewCanvasWidth, float viewCanvasHeight){
    (void)cameraTransform;
    HtmlUIContext* screen = getContext();
    if (!screen)
        return;

    // a 3D camera has no canvas, and a Screen Space - Overlay game view shows the documents in
    // screen space under any camera (Unity's overlay canvas ignores it, R57): the document fills
    // the viewport (or its part of the view's canvas)
    float w = 0.0f, h = 0.0f;
    getFrameSize(w, h);
    if (!throughCamera || camera.type == CameraType::CAMERA_PERSPECTIVE){
        screen->composite(colorPipeline, screenQuadMatrix(flipY, w, h, viewCanvasWidth, viewCanvasHeight), msaaSamples);
        return;
    }

    // the Scene view's UI and 2D cameras, and a Screen Space - Camera scene's in every view (an
    // older scene keeps the placement it had before S92: its migration gives it that mode): the
    // canvas rectangle (0,0)-(width,height) of the world
    // space where RectTransform UI lives, through the camera, so a panned or zoomed view shows
    // the document in place. The document's top edge is at y = 0 under a
    // UI camera (y down) and at y = height under an orthographic one (y up, UI flipped)
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
