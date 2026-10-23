// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIRENDERSTATS_H
#define HTMLUIRENDERSTATS_H

#include <cstdint>

namespace jasperengine {

    // One frame of HTML UI rendering (phase 1, p1-render): reset by RmlRenderer::beginFrame,
    // summed over a scene's contexts by UIDocumentSystem::getRenderStats. No RmlUi types: the
    // editor's stats overlay and phase 5 read it.
    struct HtmlUIRenderStats {
        uint32_t passes = 0;
        uint32_t drawCalls = 0;
        uint32_t pipelineApplies = 0;
        uint32_t pipelineCacheMisses = 0;   // every UI program, phase-5 effect programs included
        uint32_t textureBinds = 0;
        uint32_t layersPushed = 0;
        uint32_t filtersApplied = 0;
        uint32_t compositesFused = 0;       // backdrop composites drawn without their temporary layer (p5-tooling)
        uint32_t geometryLive = 0;
        uint32_t geometryBytes = 0;
        // S16: batching, clips, primitives and text (drawCalls are GPU draws, posts included)
        uint32_t geometrySubmitted = 0;     // RenderGeometry and RenderPrimitives calls (drawCalls before batching)
        uint32_t runsMerged = 0;            // draws that merged more than one of them
        uint32_t runCacheHits = 0;          // run chunks drawn from the last frame's gathered data
        uint32_t instancesDrawn = 0;        // HTMLUI_PRIM instances
        uint32_t verticesGathered = 0;      // vertices and instances written into the gathered stream
        uint32_t stencilDraws = 0;          // stencil-only draws: clip masks and their clears
        uint32_t stencilClipsElided = 0;    // clip masks not drawn: a scissor or the analytic clip instead
        uint32_t analyticClips = 0;         // clip mask lists clipped in the shaders
        uint32_t boxShadowTextures = 0;     // live textures of saved layers (box shadows drawn as textures)
        uint32_t glyphsRasterized = 0;      // bitmap glyphs made this frame
        uint32_t msdfGlyphsPending = 0;     // MSDF glyphs asked for, not made yet
        uint32_t textRegenerations = 0;     // text geometry generated this frame
        uint32_t textEffectFallbacks = 0;   // text effects drawn on the CPU or layer path
        uint32_t atlasBytes = 0;            // glyph atlas pages (base and MSDF), one copy each
        uint32_t atlasEvictions = 0;        // atlas pages cleared to make room, this frame

        // another context's stats added: its frame counters when it rendered in this frame, the
        // current values (geometry, saved-layer textures) always; the atlas is shared, its size is
        // the larger one
        void add(const HtmlUIRenderStats& other, bool rendered){
            if (rendered){
                passes += other.passes;
                drawCalls += other.drawCalls;
                pipelineApplies += other.pipelineApplies;
                pipelineCacheMisses += other.pipelineCacheMisses;
                textureBinds += other.textureBinds;
                layersPushed += other.layersPushed;
                filtersApplied += other.filtersApplied;
                compositesFused += other.compositesFused;
                geometrySubmitted += other.geometrySubmitted;
                runsMerged += other.runsMerged;
                runCacheHits += other.runCacheHits;
                instancesDrawn += other.instancesDrawn;
                verticesGathered += other.verticesGathered;
                stencilDraws += other.stencilDraws;
                stencilClipsElided += other.stencilClipsElided;
                analyticClips += other.analyticClips;
                glyphsRasterized += other.glyphsRasterized;
                msdfGlyphsPending += other.msdfGlyphsPending;
                textRegenerations += other.textRegenerations;
                textEffectFallbacks += other.textEffectFallbacks;
                atlasEvictions += other.atlasEvictions;
            }
            geometryLive += other.geometryLive;
            geometryBytes += other.geometryBytes;
            boxShadowTextures += other.boxShadowTextures;
            atlasBytes = (other.atlasBytes > atlasBytes) ? other.atlasBytes : atlasBytes;
        }
    };

}

#endif //HTMLUIRENDERSTATS_H
