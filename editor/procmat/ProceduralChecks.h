// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALCHECKS_H
#define PROCEDURALCHECKS_H

// The metrics and checks of a material's images (docs/specs/S90-procedural-material.md R36, R37):
// preview_procedural_material, bake_procedural_material's fail_on, check --materials and the
// Procedural Material window's Problems strip all call report(), so a human and an agent read the
// same rows. Pure: the same images and thresholds give the same rows (unit tests feed made-up ones).
//
// Images are 8-bit RGBA, the row at v = 0 first, by the bake's output names: base_color (sRGB),
// normal (the engine's convention, docs/textures.md), orm (occlusion, roughness, metallic), emission
// (sRGB) and height (in red).

#include "ProcMatQaSettings.h"
#include "ProceduralEvaluator.h"
#include "util/QaChecks.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    struct MaterialImages {
        std::map<std::string, OutputImage> outputs;
        // the shader texture of the material that samples the height (a parallax or displacement
        // input); empty when nothing does, and then height_8bit has nothing to warn about
        std::string heightReader;
    };

    struct CheckRequest {
        ProcMatQaSettings qa;
        // the check ids to run; empty: every one
        std::vector<std::string> only;
        // consecutive FAILs before this run, by check id
        std::map<std::string, int> rounds;
    };

    class ProceduralChecks {
    public:
        static const std::vector<std::string>& checkIds();

        // {tileability, repetition, histogram, base_color, normal, roughness, metallic, cost}: a block is
        // null when the images it reads are absent
        static qa::Json metrics(const MaterialImages& images);
        // the rows, in checkIds() order (rounds not counted yet)
        static qa::Json rows(const qa::Json& metrics, const MaterialImages& images, const ProcMatQaSettings& qa);
        // thresholds{values, changed}
        static qa::Json thresholds(const ProcMatQaSettings& qa);
        // Advisory (R37), no status: palette (mean CIE76 distance of the reference's dominant colours to
        // the material's), spectrum (L1 distance of the radial power spectra), edges (edge density
        // difference), of the base colour, else the first output, against the reference image
        static qa::Json reference(const MaterialImages& images, const OutputImage& reference);

        // {metrics, checks, fixes, verdict, thresholds, stop, guidance}: what every caller shows
        static qa::Json report(const MaterialImages& images, const CheckRequest& request);

        static constexpr const char* CLOSING =
            "Report these numbers; a metric cannot judge the look: look at the sheet, or ask the user, before calling it done.";
    };

}

#endif //PROCEDURALCHECKS_H
