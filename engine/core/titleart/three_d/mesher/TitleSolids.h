// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/title.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S86-title-3d.md R4, R6, R10, R11, R12, §6.2)
// - Purpose: a title's outlines -> its solids. Text splits into PARTS: every rigged glyph is a
//   part of its own (extruded alone, moved by its rig matrix), the other glyphs are unioned and
//   each resulting island is a part. The LETTER is an extrusion with one bevel placed by
//   `bevel_position` (inside: ring stack; outside / center: the outward rings of each group of
//   touching glyphs), an inflate height field or the orb. BANDS stack behind it: base outline
//   (contour / hull / box), cumulative grow, round bands per island, zFront = min(back -
//   offset%, top - (offset < 0 ? 0.25 % : 0)); a rigged glyph carries its own stack lifted 0.25 %.
// - Spans: entry 0 the layer bounds, entry 1 + g glyph g's boxes; a shape made of several glyphs
//   (merged letters, a shared band) uses one union entry. Parts record each solid's z range.
// - Tier reductions are inputs (TitleSolidsOptions); draft halves curve segments and coarsens
//   inflate and ring refinement by 2.
// - Thread-safety: pure apart from the inflate field cache it is handed (which locks).

#pragma once

#include "Export.h"
#include "titleart/geometry/Path.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/mesher/InflateMesher.h"
#include "titleart/three_d/mesher/RawMesh.h"
#include "titleart/three_d/mesher/TitleOutline3D.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace jasperengine::titleart::three_d {

    // a span-table entry
    struct SpanBox {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    struct TitlePart3D {
        Shapes shapes;
        std::vector<int64_t> owners;    // per shape: the CharAdjust key of its glyph, or -1
        uint16_t span = 0;
        int rig = -1;                   // index into TitleShapes3D::rigs
        geometry::Path outline;         // a rigged glyph's outline (the base of its own band stack)
    };

    struct GlyphMark {
        TitleVec2 p;
        uint16_t span = 0;
    };

    struct TitleShapes3D {
        std::vector<TitlePart3D> parts;
        Shapes outline;                 // the silhouette minus rigged glyphs (the shared stack's contour base)
        geometry::Path outlinePath;     // its outline paths
        geometry::Path allPath;         // every glyph, rigged ones included (hull / box bases)
        double shear = 0;
        std::vector<SpanBox> line, chr;
        std::vector<GlyphMark> marks;
        std::optional<std::array<double, 4>> orb;   // rx, ry, cx, cy
        std::vector<TitleMeshRig> rigs;
        double size = 200;
        double eps = 1e-4;
        int curveSegments = 8;
    };

    // the outlines of a source flattened at `curveSegments` and normalised (the shape cache's value)
    JASPERENGINE_API TitleShapes3D buildTitleShapes(const TitleSource3D& source, int curveSegments);

    struct TitleSolidsOptions {
        double outputScale = 1.0;       // output px per layer unit
        bool draft = false;
        int maxBevelSegments = 24;      // the tier's cap
        size_t inflateMaxNodes = 200000;
        double inflateStepScale = 1.0;  // Low: 1.5
        uint32_t testFlags = 0;         // MESHER_TEST_*
    };

    struct TitleSolids {
        RawMesh mesh;
        std::vector<TitleMeshSpan> spans;
        std::vector<TitleMeshPart> parts;
        std::vector<TitleMeshRig> rigs;
        std::vector<TitleMeshSlot> slots;   // roles per slot number (ranges filled by the builder)
        uint32_t solids = 0;
        bool fallbackUsed = false;
        uint32_t ringLevels = 0;
        float pivotZ = 0;
    };

    // the slots of the layer's block and of every character block
    JASPERENGINE_API uint32_t titleBlockSize(size_t bandCount);
    JASPERENGINE_API std::vector<TitleMeshSlot> titleSlots(size_t bandCount, const std::map<uint32_t, CharBlock3D>& blocks);

    JASPERENGINE_API TitleSolids buildTitleSolids(const TitleShapes3D& shapes, const Title3D& style, const std::map<uint32_t, CharBlock3D>& blocks,
                                                  const TitleSolidsOptions& options, InflateFieldCache* fields);

}
