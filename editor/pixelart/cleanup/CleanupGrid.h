// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/grid.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The pixel grid hidden in an upscaled picture (docs/specs/S46-pixel-art-cleanup.md R2-R4): where
// each "fake pixel" (a cell) starts and ends. A grid is a list of cuts per axis in source pixels;
// a uniform grid comes from a cell size and offset, detectGrid finds those, and refineGrid moves
// each cut onto its edges (straight or bent) for pictures whose cells drift or warp.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // Runs body(i) for every i in [0, count). Each body writes only its own slot and the callers
    // reduce the slots in index order, so the results never depend on how the work was split (R40).
    using ParallelFor = std::function<void(int count, const std::function<void(int)>& body)>;
    // The loop detection spreads its spectrum and candidates over: empty (the default) runs them in
    // order on the calling thread; the editor installs S06's job pool once at startup.
    void setParallelFor(ParallelFor parallel);

    enum class Axis : uint8_t { X, Y };

    struct GridSize {
        int width = 0, height = 0;
    };

    // The source box of the cell in column x, row y: {left, top, right, bottom}
    std::array<double, 4> cellBox(const Grid& grid, int x, int y);

    // Cuts for one axis of a uniform grid: a partial cell at either end stays when it is at least
    // half a cell; a picture smaller than one cell is one cell
    std::vector<double> uniformCuts(double length, double cell, double offset);
    Grid uniformGrid(int width, int height, const GridSpec& spec);
    // The number of cells: the fixed picture's size
    GridSize gridSize(const Grid& grid);

    // How sharply the colour changes across each column boundary (columns[x], between pixels
    // x - 1 and x) and row boundary, averaged along it; width + 1 and height + 1 entries
    struct EdgeProfiles {
        std::vector<double> columns, rows;
    };
    EdgeProfiles edgeProfiles(const RgbaImage& image);

    // The cell size and offset on one axis from its edge profile (R3); nullopt without edges.
    // `prefer` is a size to choose when it is nearly as clean as the cleanest (the other axis's).
    std::optional<AxisFit> detectAxis(const RgbaImage& image, Axis axis, const std::vector<double>& profile,
                                      double maxCell = 128, std::optional<double> prefer = std::nullopt);
    // The grid of an upscaled pixel-art picture; nullopt when it has no edges at all
    std::optional<Detection> detectGrid(const RgbaImage& image, double maxCell = 128);

    // How much a cut at each position would split a dark stroke: the share of lines (rows from
    // `from` to `to` for X; to < 0: all) where the pixels on both sides of the boundary are outline
    std::vector<double> crossingProfile(const OutlineMask& mask, Axis axis, int from = 0, int to = -1);

    // Moves the inner cuts onto edges (a dynamic program over candidates within half a cell);
    // border cuts never move and a sliver left at either end joins its neighbour
    std::vector<double> refineCuts(const std::vector<double>& profile, const std::vector<double>& cuts, double length,
                                   const std::vector<double>* crossings = nullptr);

    struct RefineOptions {
        bool bend = false;                    // place the cuts in every row and column of cells on its own
        const OutlineMask* outlines = nullptr;  // lines keep off its dark strokes
    };
    Grid refineGrid(const RgbaImage& image, const Grid& grid, const RefineOptions& options = {});

    // Where a grid of a known cell size starts in a picture (frames of one sheet)
    struct GridOffsets {
        double offsetX = 0, offsetY = 0;
    };
    GridOffsets fitOffsets(const RgbaImage& image, double cellWidth, double cellHeight);

}
