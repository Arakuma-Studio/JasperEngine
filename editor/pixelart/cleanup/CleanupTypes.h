// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core: fix.ts, grid.ts, outline.ts, sheet.ts,
// animation.ts, lost.ts, edits.ts, cleanup.ts, background.ts, recipe.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The plain data of the pixel-art cleanup (docs/specs/S46-pixel-art-cleanup.md §4.1): the fixed
// picture (PixelArt), grids, cell samples, detections, sheets and the recipe. Every module of
// editor/pixelart/cleanup/ shares these; the functions live in each module's header. Names follow
// ritmo's TypeScript (camelCase members, the same meaning), so a ported test reads like the
// original. Pure data, no project or editor dependency; any thread.

#pragma once

#include "pixelart/PixelColor.h"
#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Rgba8;

    // ritmo's RgbaImage (the browser's ImageData): straight-alpha RGBA8 rows, top-left first
    using RgbaImage = PixelImage;

    // ---- fix.ts

    constexpr int32_t TRANSPARENT = -1;

    // The fixed picture: one palette index per pixel, or TRANSPARENT. Palette colours are opaque
    // (alpha 255; ritmo's Rgb has none).
    struct PixelArt {
        int width = 0;
        int height = 0;
        std::vector<Rgba8> palette;
        std::vector<int32_t> indices;   // width * height, rows top first

        bool operator==(const PixelArt& other) const = default;
    };

    // The pixels read from one cell: the opaque ones packed 0xRRGGBB (pixelart::packRgb), and how
    // many read were transparent
    struct Cell {
        std::vector<uint32_t> colors;
        int transparent = 0;
    };

    // The cells of a grid, rows top first
    struct CellSamples {
        int width = 0;
        int height = 0;
        std::vector<Cell> cells;
    };

    struct SampleOptions {
        double inset = 0.25;          // share of each side of a cell left out, 0-0.45
        double alphaThreshold = 128;  // alpha below this counts as transparent

        bool operator==(const SampleOptions& other) const = default;
    };

    enum class Dither : uint8_t { None, Ordered };

    // ---- grid.ts

    struct GridSpec {
        double cellWidth = 0, cellHeight = 0, offsetX = 0, offsetY = 0;
        bool operator==(const GridSpec& other) const = default;
    };

    // Bent lines: xs[i][j] is column cut i where it crosses row j of cells, ys[i][j] row cut i in column j
    struct GridBend {
        std::vector<std::vector<double>> xs, ys;
        bool operator==(const GridBend& other) const = default;
    };

    // Cut positions on each axis, ascending, in source pixels (fractions allowed)
    struct Grid {
        std::vector<double> xs, ys;
        std::optional<GridBend> bend;
        bool operator==(const Grid& other) const = default;
    };

    struct AxisFit {
        double cell = 0, offset = 0, confidence = 0;
    };

    struct Detection {
        GridSpec spec;
        AxisFit x, y;
        double confidence = 0;   // the weaker axis
    };

    // Outline pixels (outline.ts's OutlineMask, grid.ts's DarkMask): 1 where a pixel is one
    struct OutlineMask {
        int width = 0, height = 0;
        std::vector<uint8_t> dark;
        int threshold = 0;       // the darkest luminance (0-255) that still counts as outline
    };

    // ---- reduce.ts, cleanup.ts, background.ts, lost.ts, edits.ts

    enum class ReduceMode : uint8_t { Common, Outlines };

    struct BackgroundOptions {
        double tolerance = 0.08;
        bool enclosed = false;
        bool specks = true;
        bool shadow = false;
        bool operator==(const BackgroundOptions& other) const = default;
    };

    struct LostColor {
        Rgba8 color;
        Rgba8 became;
        std::vector<int> cells;  // grid cells, rows top first
        int frame = -1;          // a sheet's frame (0-based), -1 for a single picture
    };

    enum class EditTool : uint8_t { Erase, Restore, Paint };

    struct Edit {
        int x = 0, y = 0;
        bool area = false;
        EditTool tool = EditTool::Erase;
        Rgba8 color;             // paint only
        bool operator==(const Edit& other) const = default;
    };

    // ---- sheet.ts

    struct FrameBox {
        int x = 0, y = 0, width = 0, height = 0;
        bool operator==(const FrameBox& other) const = default;
    };

    enum class Anchor : uint8_t { Ground, Feet, Center, Box };
    enum class Loop : uint8_t { Forward, PingPong };

    struct Point {
        int x = 0, y = 0;
        bool operator==(const Point& other) const = default;
    };

    struct SheetFrame {
        FrameBox box;
        bool include = true;
        Point nudge;
        bool operator==(const SheetFrame& other) const = default;
    };

    constexpr double DEFAULT_FPS = 8;   // sheet.ts

    struct SheetAnimation {
        std::string name;
        std::vector<int> frames;   // indices into Sheet::frames, in play order
        bool include = true;
        double fps = DEFAULT_FPS;
        Loop loop = Loop::Forward;
        bool operator==(const SheetAnimation& other) const = default;
    };

    struct Sheet {
        std::vector<SheetFrame> frames;
        std::vector<SheetAnimation> animations;
        Anchor anchor = Anchor::Ground;
        int padding = 1;
        bool operator==(const Sheet& other) const = default;
    };

    // ---- recipe.ts (R28)

    enum class GridKind : uint8_t { Auto, Cells, Size };

    struct GridChoice {
        GridKind kind = GridKind::Auto;
        GridSpec spec;                       // Cells
        double width = 0;                    // Size
        std::optional<double> height;        // Size; nullopt keeps the aspect ratio
        bool operator==(const GridChoice& other) const = default;
    };

    enum class Follow : uint8_t { Off, Straight, Bend };
    enum class Outlines : uint8_t { Auto, On, Off };

    struct RecipePalette {
        std::string name;
        std::vector<Rgba8> colors;
        std::vector<int> locked;             // indices into colors
        // an S43 .palette asset the colours came from ("" when none): stored so a re-run can say where
        std::string assetGuid, assetPath;
        bool operator==(const RecipePalette& other) const = default;
    };

    // How the recipe asks for its palette before it is resolved: the auto proposal (R9)
    enum class ProposeMethod : uint8_t { Dominant, Balanced, Distinct };

    struct AutoPalette {
        int count = 16;
        ProposeMethod method = ProposeMethod::Dominant;
        bool operator==(const AutoPalette& other) const = default;
    };

    struct SpeckCleanup {
        double size = 0, strength = 0.5;
        bool operator==(const SpeckCleanup& other) const = default;
    };

    struct SizedCells {
        std::string size;                    // "WxH" the cells were recorded at
        std::vector<int> cells;
        bool operator==(const SizedCells& other) const = default;
    };

    struct SizedEdits {
        std::string size;                    // "WxH" the strokes were recorded at
        std::vector<std::vector<Edit>> strokes;
        bool operator==(const SizedEdits& other) const = default;
    };

    enum class SheetMode : uint8_t { Auto, Off, Given };

    enum class OutputFormat : uint8_t { Png, Aseprite };

    struct RecipeSource {
        std::string path;                    // project-relative (or as given to the CLI)
        std::string sha256;                  // of the source file's bytes
        std::optional<int> frame;            // an .aseprite source's frame (1-based)
        std::string layer;                   // an .aseprite source's layer ("" all visible)
        bool operator==(const RecipeSource& other) const = default;
    };

    // Default: the key was absent; None: "none" (own colours); Auto: proposed from the picture;
    // Fixed: given colours, a preset, or an S43 asset (RecipePalette::assetPath/assetGuid)
    enum class PaletteMode : uint8_t { Default, None, Auto, Fixed };

    struct Recipe {
        static constexpr const char* kFormat = "jasperengine.pixel-cleanup-recipe";
        static constexpr int kVersion = 1;
        static constexpr int kPipelineVersion = 1;

        GridChoice grid;
        Follow follow = Follow::Off;
        Outlines outlines = Outlines::Auto;
        std::vector<std::pair<std::string, double>> moved;   // ["x:3", 41.5]
        SampleOptions sample;
        // How the palette was asked for (R9). The pipeline reads only `palette`: the session resolves
        // Default (the project's Default Palette, else auto), Auto (proposed from the picture) and an
        // unresolved asset into `palette`'s colours before running, so a resolved recipe re-runs alone.
        PaletteMode paletteMode = PaletteMode::Default;
        // nullopt: each cell keeps its own colour
        std::optional<RecipePalette> palette;
        // set while the palette is still to be proposed from the picture (resolved into palette)
        std::optional<AutoPalette> autoPalette;
        jasperengine::pixelart::ColorMetric metric = jasperengine::pixelart::ColorMetric::OkLab;
        Dither dither = Dither::None;
        double ditherStrength = 0.5;
        SpeckCleanup cleanup;
        SizedCells cleanKeep;
        std::optional<double> targetWidth;
        ReduceMode reduceMode = ReduceMode::Outlines;
        bool removeBackground = false;
        bool crop = false;
        BackgroundOptions background;
        double lostThreshold = 0.06;
        SizedEdits edits;
        SheetMode sheetMode = SheetMode::Auto;
        std::optional<Sheet> sheet;          // SheetMode::Given (or resolved from Auto)

        // S46's additions (R14, R28, R30)
        double alphaCutoff = 128;
        uint32_t seed = 1;
        RecipeSource source;
        std::string resultSha256;
        std::optional<OutputFormat> format;
        bool forced = false;
        std::string engineVersion;
        int pipelineVersion = kPipelineVersion;

        bool operator==(const Recipe& other) const = default;
    };

}
