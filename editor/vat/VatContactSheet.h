// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 6: the preview's contact sheet)
// - Purpose: the image bake_vertex_animation returns with a preview: per clip (up to
//   VAT_SHEET_MAX_CLIPS) a row of the source and a row of the VAT at 4 times (a loop clip at
//   0, 1/4, 1/2, 3/4 of its length; other wraps at 0, 1/3, 2/3 and the end), so an agent sees
//   whether the bake matches. Imports have no source: VAT rows only.
// - Both rows are drawn the same way, offscreen on the CPU (a z-buffered rasterizer, 2x2
//   supersampled, two-sided Lambert shading of the face normals, the materials' base colours),
//   from one framing per clip (the union of both rows' bounds, seen from the front-right and a
//   little above, +Z towards the viewer at yaw 0, Y up): the differences between the rows are the
//   bake's, not the renderer's, and it works without a GPU (the CLI, headless tests).
//   * the source row: the hierarchy posed by VatPoser at each time (ActionSystem's preview, the
//     scene restored afterwards), each renderer deformed by cpuskin::cpuDeform (skin, morphs,
//     lattices) and moved into the root's space, exactly what the bake samples;
//   * the VAT row: the asset's own payload decoded with the shader's frame math (vatSample: the
//     two frames and their mix; Vertices: rest + offset, or the absolute positions of a "rest":
//     "none" clip; Bones: nlerped rotation, lerped translation and scale, weighted per vertex),
//     i.e. what the GPU draws at that clip time.
// - Thread-safety: main thread (the source row poses the scene). Pure otherwise.

#ifndef VAT_CONTACT_SHEET_H
#define VAT_CONTACT_SHEET_H

#include "vat/VatAssetWriter.h"
#include "vat/VatBakeSource.h"

#include "animation/VertexAnimation.h"
#include "math/Vector3.h"
#include "pose/PoseImage.h"

#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    constexpr int VAT_SHEET_TIMES = 4;
    constexpr int VAT_SHEET_MAX_CLIPS = 6;

    // a triangle soup to draw: positions, triangles, one colour per triangle
    struct VatSheetMesh{
        std::vector<jasperengine::Vector3> positions;
        std::vector<uint32_t> indices;
        std::vector<PoseColor> colors;      // per triangle (indices.size() / 3)
    };

    // the 4 sample times of a clip of `length` seconds
    std::vector<float> vatSheetTimes(float length, ClipWrapMode wrap);

    // The VAT's mesh at clip-local time `time` of clip `clip` (object space). False with `error`
    // for a payload that doesn't hold the layout or a bad clip index.
    bool vatMeshAt(const jasperengine::vat::VatData& data, const jasperengine::vat::VatPayload& payload, const VatMeshData& mesh,
                   int clip, float time, VatSheetMesh& out, std::string* error = nullptr);

    // The source's mesh posed by `clip` at `time` (root space) through an begun poser.
    bool sourceMeshAt(VatPoser& poser, const VatSourceClip& clip, float time, VatSheetMesh& out, std::string* error = nullptr);

    // One cell: the mesh drawn from the sheet's view, framed on [min, max]
    PoseImage renderVatCell(const VatSheetMesh& mesh, const jasperengine::Vector3& min, const jasperengine::Vector3& max, int width, int height);

    struct VatSheetRow{
        std::string label;                  // "Walk source", "Walk VAT"
        std::vector<VatSheetMesh> meshes;   // one per time
        int frameGroup = 0;                 // rows of a group share a framing (one clip's)
    };

    // The sheet: rows of VAT_SHEET_TIMES cells, each with its label "<row label> t=<time>"
    PoseImage composeVatSheet(const std::vector<VatSheetRow>& rows, const std::vector<std::vector<float>>& times,
                              const std::string& title, int cellSize);

}

#endif //VAT_CONTACT_SHEET_H
