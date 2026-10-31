// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.7, 6)
// - Purpose: the one problem and report shape of every Vertex Animation producer, the baker
//   (VatBakeSession) and the importers (import/VatImport.h: VatImportReport extends VatReport),
//   so the window, the Inspector, MCP and the CLI show and print them the same way.
//   * VatIssue: {kind, message, fix} (+ error, + fixes as actions); errors refuse the bake or
//     import, warnings don't.
//   * VatReport: {mode, mode_reason, vertices, bones, clips[{name, frames, fps, bytes}],
//     texture{width, height, format "rgba16f" | "rgba32f"}, bytes, max_error_mm, mean_error_mm,
//     quantization_error_mm, tolerance_mm, fetches_per_vertex, warnings[]} (reportJson).
// - Thread-safety: plain values and pure functions.

#ifndef VAT_REPORT_H
#define VAT_REPORT_H

#include "animation/VertexAnimation.h"

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    // One action that resolves (part of) an issue: a button in the window, a hint in MCP.
    // `setting` names the spec field ("fps", "normals", "mode", "clips", "preset", "precision",
    // "interpolate", "output", "overwrite", "source"), `value` the value to set (JSON text).
    struct VatFix{
        std::string label;                  // "Lower to 20 fps"
        std::string setting;
        std::string value;
    };

    struct VatIssue{
        std::string kind;
        std::string message;                // one sentence, names the clip/file/node concerned
        std::string fix;                    // one sentence (the fixes' labels for too_large)
        bool error = false;
        std::vector<VatFix> fixes;          // ordered by effect (too_large), may be empty
    };
    bool hasErrors(const std::vector<VatIssue>& issues);
    // "kind: message (fix)" lines
    std::string issuesText(const std::vector<VatIssue>& issues, bool errorsOnly = false);

    struct VatClipReport{
        std::string name;
        int frames = 0;
        float fps = 0.0f;
        uint64_t bytes = 0;
        float maxErrorMm = 0.0f;            // at its frames and mid-frames (baker)
        float interpolationErrorMm = 0.0f;  // at its mid-frames (baker)
    };

    struct VatReport{
        jasperengine::vat::VatMode mode = jasperengine::vat::VatMode::Vertices;
        std::string modeReason;
        int vertices = 0;                   // payload vertices (Vertices) / mesh vertices (Bones)
        int bones = 0;
        std::vector<VatClipReport> clips;
        int width = 0;
        int height = 0;
        jasperengine::vat::VatPrecision precision = jasperengine::vat::VatPrecision::Half;
        uint64_t bytes = 0;
        float maxErrorMm = 0.0f;
        float meanErrorMm = 0.0f;
        float quantizationErrorMm = 0.0f;   // Vertices: half vs exact at the frames (0 for float)
        float toleranceMm = 0.0f;           // the tolerance used (scaled by the bounds)
        int fetchesPerVertex = 0;
        std::vector<VatIssue> warnings;
    };

    // "rgba16f" | "rgba32f"
    const char* textureFormatName(jasperengine::vat::VatPrecision precision);

    nlohmann::json issueJson(const VatIssue& issue);
    nlohmann::json issuesJson(const std::vector<VatIssue>& issues);
    nlohmann::json reportJson(const VatReport& report);

}

#endif //VAT_REPORT_H
