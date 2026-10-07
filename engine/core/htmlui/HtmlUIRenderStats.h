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
        uint32_t geometryLive = 0;
        uint32_t geometryBytes = 0;
    };

}

#endif //HTMLUIRENDERSTATS_H
