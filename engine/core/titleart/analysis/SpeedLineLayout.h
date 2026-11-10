// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Ported from Pachislop src/generators/lines/speedLines.ts by SafEight, used with the author's permission.

// Contract (docs/specs/S83-title-effects-generators.md §6.7.3, R23, AC5, AC11)
// - Purpose: Speed Lines' CPU layout (集中線 / 流線 / フラッシュ / ベタフラ), Pachislop's layout()
//   with its maths, constants and seeded stream kept: the focus boundary sampled at 1,024 angles
//   (or across the frame for flow lines) from an analytic ellipse or rectangle, or from the
//   followed layer's silhouette (alpha marched on its 6 px grid, the last exit taken, a running
//   max over +-2 samples closing the notches between glyphs); stratified slots with clumps and
//   gaps, jagged teeth, frame-anchored lengths, one shared curvature split into a quad per 2 deg
//   of turn (at most 32, inner joints never overlapping) and six draws per line whatever its
//   options, so toggling one keeps the other lines in place.
// - Spaces: everything is in the layout space (Pachislop's layer space: canvas px, +y up);
//   `frame` is the whole render's corners in it, `k` the target px per layout px. A silhouette
//   grid (analysis/ReadGrid) is in canvas px and `spaceToCanvas` maps the layout space onto it;
//   its march step is in canvas terms, so layouts at any px_scale agree in canvas px (AC5).
// - Determinism: a pure function of its input (gen::Rng streams, no clock).
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/gpu/QuadBatch.h"

#include <cstdint>
#include <vector>

namespace jasperengine::titleart::analysis {

    constexpr int SPEED_LINE_TABLE = 1024;      // samples of the focus boundary
    constexpr int SPEED_LINE_MAX = 600;         // lines at most
    constexpr double SPEED_LINE_CELL = 6.0;     // the silhouette grid's cell, canvas px

    enum class SpeedLineType : uint8_t { Focus, Flow, Flash, Beta };
    enum class SpeedLineShape : uint8_t { Ellipse, Rect, Layer };
    enum class SpeedLineSide : uint8_t { Both, Front, Back };
    enum class SpeedLineFill : uint8_t { Solid, Length, Around };

    // The params in Pachislop's units (percent where it keeps percent, its counter-clockwise flow
    // angle) and where the layout runs
    struct SpeedLineInput {
        SpeedLineType type = SpeedLineType::Focus;
        TitleVec2 center;                   // where the lines converge, layout px
        SpeedLineShape shape = SpeedLineShape::Ellipse;
        double padding = 40.0;              // around the silhouette, layout px
        TitleVec2 radius{420.0, 260.0};     // the ellipse's or rectangle's half sizes
        double angle = 0.0;                 // flow direction, radians counter-clockwise (+y up)
        SpeedLineSide side = SpeedLineSide::Both;
        double count = 160.0;
        double width = 10.0;                // layout px
        double widthRandom = 60.0;          // percent
        double length = 100.0;              // percent of the way to the frame edge (or of the focus radius)
        double lengthRandom = 0.0;          // percent
        double jitter = 60.0;               // layout px
        double jagCount = 0.0;
        double jagHeight = 40.0;            // layout px
        double clump = 1.0;
        double gap = 0.0;                   // empty slots between clumps
        double curve = 0.0;                 // percent, -100..100
        SpeedLineFill fill = SpeedLineFill::Solid;
        TitleColor color{1.0f, 1.0f, 1.0f}; // linear
        TitleGradient gradient;             // Around: sampled per line
        double opacity = 85.0;              // percent
        double seed = 1.0;                  // the element seed (gen::seedOf)

        std::vector<TitleVec2> frame;       // the whole render's corners in order around it, layout px
        double k = 1.0;                     // target px per layout px
        // a Layer focus: the followed layer's grid and the layout space -> canvas px; without a
        // grid the ellipse stands in (Pachislop's missing focus layer)
        const Grid* silhouette = nullptr;
        TitleAffine spaceToCanvas;
    };

    // One line (a tapered wedge) before it is cut into quads
    struct SpeedLine {
        int slot = 0;           // the line's index among `count`
        int side = 0;           // flow: +1 ahead of the focus, -1 behind, 0 a sweep across the frame; radial: 0
        double x = 0.0, y = 0.0;        // its focus end, layout px
        double heading = 0.0;           // radians counter-clockwise at that end
        double length = 0.0;            // arc length, layout px
        double halfWidth = 0.0;         // layout px; negative: no focus end (tapers like the far end on both)
        float color[4] = {0, 0, 0, 0};  // premultiplied linear
    };

    struct SpeedLineLayout {
        std::vector<SpeedLine> lines;
        // the QuadBatch records (titleart/gen/speed_lines.glsl): extra = (t at the quad's two
        // ends, half width and length in target px)
        std::vector<gen::QuadInstance> quads;
        // radial types: per angle sample (2 pi i / 1024) the focus boundary plus teeth and the
        // frame exit, layout px from the centre, as RGBA32F texels (z, w unused); empty for flow
        std::vector<float> table;
        TitleVec2 center;
        double curvature = 0.0;         // every line's, per layout px
    };

    JASPERENGINE_API SpeedLineLayout layoutSpeedLines(const SpeedLineInput& in);

    // the point at arc length s along a line (its arc of the layout's curvature)
    JASPERENGINE_API TitleVec2 speedLinePoint(const SpeedLine& line, double curvature, double s);

    // The box of a grid's ink in canvas px (Pachislop's layerBounds, the followed layer's centre
    // of Speed Lines and Sunburst): the cells holding any alpha, each outer edge moved into its
    // cell by the coverage that row or column reaches (a straight edge across it); false when
    // the grid is empty
    JASPERENGINE_API bool gridInkBox(const Grid& grid, double& x0, double& y0, double& x1, double& y1);

}
