// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R8, R35)
// - Purpose: what an export does with Title Art. A `.titleart` is not shipped (its baked PNGs
//   ship as ordinary textures through S14) unless isNeeded says a shipped scene or prefab
//   renders it in Realtime or Glyph Atlas mode (S81 fills isNeeded; until then it is false).
// - Before the textures cook, prepare() applies title_art.stale_bakes_on_export to every
//   document's outputs that are stale or missing: `rebake` bakes them first, `warn` lists them,
//   `fail` fails the export naming them. Documents of a newer version are listed, never rebaked.
// - Thread-safety: main thread (the exporter calls it through a main-thread task).

#pragma once

#include "TitleArtSettings.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    namespace TitleArtExport {

        // the document (assets-relative) is read at runtime by a shipped Realtime or Glyph Atlas title
        bool isNeeded(Project* project, const std::string& assetsRelativePath);
        // S81: the export's plan names those documents (project-relative) before the assets copy
        void setRuntimeTitles(Project* project, const std::vector<std::string>& projectRelativePaths);

        struct Prepared {
            bool ok = true;
            std::string error;                  // `fail`: names the documents and outputs
            std::vector<std::string> warnings;  // `warn` (and rebakes that could not run)
            std::vector<std::string> rebaked;   // "<document> (<outputs>)"
            size_t documents = 0;
        };
        Prepared prepare(Project* project, TitleStaleBakesPolicy policy);

    }

}
