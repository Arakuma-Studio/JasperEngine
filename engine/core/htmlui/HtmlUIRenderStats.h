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
        uint32_t regionLayers = 0;          // layers pushed covering more than the viewport (a filter's input beyond it, P7-F2)
        uint32_t regionTargetsCreated = 0;  // their images made (none while the pooled ones fit)
        uint32_t clipMasksRedrawn = 0;      // clip masks drawn again into another layer's depth-stencil (P7-F2)
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
        // S92: frame reuse, layer cost and memory
        uint32_t framesKept = 0;            // 1: the frame matched the last drawn one and was not replayed (R1)
        uint32_t liveSources = 0;           // why the frame was drawn whatever it recorded (RmlRenderer::LiveSource bits)
        uint32_t keptMismatches = 0;        // kept frames that differed from a fresh replay (JASPER_HTMLUI_VERIFY_KEPT), so far
        uint32_t compositeTiles = 0;        // 64x64 tiles of the document target the composite draws
        uint32_t compositePixels = 0;       // target pixels the composite draws
        uint32_t layerPixels = 0;           // render area of the layer and post-process passes, target px
        uint32_t layerCacheHits = 0;        // layer scopes drawn from the result cache
        uint32_t layerCacheMisses = 0;      // layer scopes drawn and kept in it
        uint32_t layerCacheBytes = 0;       // the result cache's textures
        uint32_t samples = 0;               // MSAA samples of the document target
        uint32_t targetBytes = 0;           // the document target's images (layers, post-process and region targets)
        uint32_t depthStencilStores = 0;    // passes that stored a depth-stencil attachment

        // every field by its name (the MCP render_stats, Lua's UIDocument:getRenderStats, the
        // benchmark's --ui-stats): fn(const char* name, uint32_t value)
        template<class F>
        void forEachField(F&& fn) const{
            fn("passes", passes);
            fn("drawCalls", drawCalls);
            fn("pipelineApplies", pipelineApplies);
            fn("pipelineCacheMisses", pipelineCacheMisses);
            fn("textureBinds", textureBinds);
            fn("layersPushed", layersPushed);
            fn("filtersApplied", filtersApplied);
            fn("compositesFused", compositesFused);
            fn("regionLayers", regionLayers);
            fn("regionTargetsCreated", regionTargetsCreated);
            fn("clipMasksRedrawn", clipMasksRedrawn);
            fn("geometryLive", geometryLive);
            fn("geometryBytes", geometryBytes);
            fn("geometrySubmitted", geometrySubmitted);
            fn("runsMerged", runsMerged);
            fn("runCacheHits", runCacheHits);
            fn("instancesDrawn", instancesDrawn);
            fn("verticesGathered", verticesGathered);
            fn("stencilDraws", stencilDraws);
            fn("stencilClipsElided", stencilClipsElided);
            fn("analyticClips", analyticClips);
            fn("boxShadowTextures", boxShadowTextures);
            fn("glyphsRasterized", glyphsRasterized);
            fn("msdfGlyphsPending", msdfGlyphsPending);
            fn("textRegenerations", textRegenerations);
            fn("textEffectFallbacks", textEffectFallbacks);
            fn("atlasBytes", atlasBytes);
            fn("atlasEvictions", atlasEvictions);
            fn("framesKept", framesKept);
            fn("liveSources", liveSources);
            fn("keptMismatches", keptMismatches);
            fn("compositeTiles", compositeTiles);
            fn("compositePixels", compositePixels);
            fn("layerPixels", layerPixels);
            fn("layerCacheHits", layerCacheHits);
            fn("layerCacheMisses", layerCacheMisses);
            fn("layerCacheBytes", layerCacheBytes);
            fn("samples", samples);
            fn("targetBytes", targetBytes);
            fn("depthStencilStores", depthStencilStores);
        }

        // another context's stats added: its frame counters when it rendered in this frame, the
        // current values (geometry, saved-layer textures, caches, targets, mismatches so far)
        // always; the atlas is shared, its size is the larger one; the live-source reasons are
        // joined, the sample count is the largest
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
                regionLayers += other.regionLayers;
                regionTargetsCreated += other.regionTargetsCreated;
                clipMasksRedrawn += other.clipMasksRedrawn;
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
                framesKept += other.framesKept;
                liveSources |= other.liveSources;
                compositeTiles += other.compositeTiles;
                compositePixels += other.compositePixels;
                layerPixels += other.layerPixels;
                layerCacheHits += other.layerCacheHits;
                layerCacheMisses += other.layerCacheMisses;
                depthStencilStores += other.depthStencilStores;
            }
            geometryLive += other.geometryLive;
            geometryBytes += other.geometryBytes;
            boxShadowTextures += other.boxShadowTextures;
            keptMismatches += other.keptMismatches;
            layerCacheBytes += other.layerCacheBytes;
            targetBytes += other.targetBytes;
            samples = (other.samples > samples) ? other.samples : samples;
            atlasBytes = (other.atlasBytes > atlasBytes) ? other.atlasBytes : atlasBytes;
        }
    };

}

#endif //HTMLUIRENDERSTATS_H
