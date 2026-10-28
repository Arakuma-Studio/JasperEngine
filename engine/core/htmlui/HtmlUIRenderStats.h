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
    };

}

#endif //HTMLUIRENDERSTATS_H
