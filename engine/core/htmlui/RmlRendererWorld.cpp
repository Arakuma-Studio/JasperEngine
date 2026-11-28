// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A World Space document's quad (S93 R36): the composited layer of its own context drawn inside
// the scene's camera pass, among the transparent objects, tested against the scene's depth
// without writing it. Kept apart from the screen composite (drawComposite).

#include "RmlRenderer.h"
#include "RmlRendererInternal.h"

using namespace jasperengine;

bool RmlRenderer::drawWorldQuad(TextureRender& source, const RenderTargetFormat& target, const Rml::Matrix4f& clipFromQuad){
    if (!isReady() || !source.isCreated())
        return false;

    // the layer is premultiplied, as the screen composite's; no culling: both faces show, the
    // back one mirrored (Unity's UI/Default)
    PipelineState state;
    state.target = target;
    state.blend = BlendMode::PREMULTIPLIED;
    state.depthCompare = CompareFunc::LESS_EQUAL;
    state.depthWrite = false;
    if (!compositeMsaaObject.beginDraw(state))
        return false;

    compositeMsaaObject.addTexture(geometryTextureSlot, ShaderStageType::FRAGMENT, &source);
    HtmlUIParams params = makeParams(clipFromQuad, Rml::Vector2f(0.0f, 0.0f));
    // htmlui.vert keeps the quad's depth
    params.translation[3] = 1.0f;
    compositeMsaaObject.applyUniformBlock(geometryParamsSlot, sizeof(HtmlUIParams), &params);
    compositeMsaaObject.applyUniformBlock(geometryClipSlot, sizeof(ClipParams), &noClip);
    compositeMsaaObject.draw(0, 6, 1);
    return true;
}
