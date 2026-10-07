// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFILTERCONTEXT_H
#define RMLFILTERCONTEXT_H

#include "Export.h"

#include <RmlUi/Core/Types.h>

#include "render/ObjectRender.h"
#include "render/PipelineState.h"

#include <functional>

namespace jasperengine {

    class Framebuffer;
    class RmlRenderer;

    // The filter chain of one RmlRenderer::CompositeLayers call: what every phase-1 filter pass
    // (opacity, blur, drop-shadow, colour matrix, mask image) is written against, and what
    // phase-5 filter effects use. Targets are 1-sample RGBA8 images of the layer size with
    // premultiplied content, rows top-down on every backend. A pass reads primary() and writes
    // a free target, which swap() then makes the source of the next filter. primary() may be
    // the source layer itself: it is only read, never written (backdrop-filter reads the layer
    // it draws on), so in-place work goes to secondary()/tertiary() first.
    class JASPERENGINE_INTERNAL RmlFilterContext {
    public:
        Framebuffer& primary();     // current filter source (the post source)
        Framebuffer& secondary();   // free target
        Framebuffer& tertiary();    // second free target (blur temp, drop-shadow)
        void swap();                // secondary becomes primary; free targets are picked again
        // Pas copy, blend NONE, current scissor
        void blit(Framebuffer& src, Framebuffer& dst);
        // RmlUi's GL3 blur (downscale passes, 7-tap separable gaussian, upscale) of the scissor
        // region; target must not be the source layer
        void blurInPlace(Framebuffer& target, Framebuffer& temp, float sigmaDevicePx);
        Rml::Rectanglei getScissor() const;            // target px, top-left origin
        Rml::Vector2i getLayerSize() const;            // rows top-down on every backend
        // fullscreen triangle (htmlui_post.vert convention) into dst with the given program and
        // state, within the current scissor; applyUniforms is called after beginDraw so the
        // caller uploads its uniform blocks and textures. load: CLEAR (default) or LOAD
        void drawFullscreen(Framebuffer& dst, ObjectRender& program, const PipelineState& state,
                            const std::function<void(ObjectRender&)>& applyUniforms,
                            LoadAction load = LoadAction::CLEAR);

        // the state drawFullscreen expects for a post target (1 sample, no depth)
        PipelineState postState(BlendMode blend = BlendMode::NONE) const;
        RmlRenderer& getRenderer();
        bool isPrimaryLayer() const;               // primary() is still the source layer

    private:
        friend class RmlRenderer;
        RmlFilterContext(RmlRenderer& renderer, Framebuffer& sourceLayer);
        void pickFreeTargets();

        RmlRenderer& renderer;
        Framebuffer* sourceLayer;
        int primaryIndex = -1;      // -1: the source layer, else a postprocess index (0..2)
        int secondaryIndex = 0;
        int tertiaryIndex = 1;
    };

}

#endif //RMLFILTERCONTEXT_H
