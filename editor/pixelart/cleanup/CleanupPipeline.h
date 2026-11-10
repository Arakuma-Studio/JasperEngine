// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/pipeline.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The whole fix, from a picture and a recipe to the fixed pixel art (docs/specs/S46-pixel-art-cleanup.md
// R18), in ritmo's order: grid, outline mask, lines, sampling, snap, thin outlines, own colours and
// lost colours, specks, shrink, background, edits, then S46's alpha cutoff and the crop. A cache
// keeps the outline mask, the base grid and the samples between runs, so a slider change re-runs
// only what it affects (R39).
//
// The background is found in the cells' own colours, not the palette's: a palette with no colour
// close to the background snaps it to the nearest one (often the outline), and a background found
// there would take the outline with it.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // Steps kept between runs of one picture: each is reused while its key (and the picture,
    // recognised by its size and PixelImage::hash) is unchanged
    struct PipelineCache {
        uint64_t maskImage = 0;
        bool hasMask = false;
        OutlineMask mask;

        uint64_t gridImage = 0;
        std::string gridKey;
        bool hasGrid = false;
        Grid baseGrid;

        bool hasSamples = false;
        uint64_t samplesImage = 0;
        Grid samplesGrid;
        std::string samplesKey;
        CellSamples samples;

        // R39: the cells snapped to the palette (before outline thinning) and the cells' own colours,
        // kept with the samples they were read from, so a slider past the snap re-runs no vote
        bool hasSnap = false;
        std::string snapKey;
        PixelArt snapped;
        bool hasOwn = false;
        std::string ownKey;
        PixelArt own;
    };

    struct Outcome {
        GridSpec spec;                        // the grid's cell size and offset, as found or given
        std::optional<Detection> detection;   // what auto-detection found, when the recipe asked for it
        Grid grid;
        bool outlines = false;                // the outline handling ran
        CellSamples samples;
        PixelArt art;
        PixelArt exported;                    // `art`, cropped when the recipe asks: what is written and linted
        std::vector<LostColor> lost;          // empty without a palette
        std::vector<std::vector<int>> specks; // the specks cleaned, each as its cells
        std::vector<Rgba8> backgroundFound;   // the background colours, when removing the background
        std::vector<int> painted;             // pixels painted by hand
        bool editsStale = false;              // hand edits were made at another size and left out
        bool cleanKeepStale = false;
        // S46: the palette the colours snapped to (an auto palette proposed here), and the source
        // pixels with 0 < alpha < 255 (R14: the output has none)
        std::optional<RecipePalette> palette;
        int partialAlpha = 0;
    };

    struct ResolvedGrid {
        GridSpec spec;
        std::optional<Detection> detection;
    };
    // The grid's cell size and offset for a choice; nullopt when Auto finds no grid
    std::optional<ResolvedGrid> resolveGrid(const RgbaImage& image, const GridChoice& choice);

    // The picture's outline pixels (cached), and whether the recipe's outline handling applies to it
    // with cells of this size (never under 2 px)
    bool outlinesOf(const RgbaImage& image, const GridSpec& spec, const Recipe& recipe, PipelineCache& cache);

    // The grid's cuts: uniform, moved onto edges when following (kept off dark strokes with
    // `outlines`), then the lines dragged by hand (`recipe.moved`; a bent line moves as a whole)
    Grid buildGrid(const RgbaImage& image, const GridSpec& spec, const Recipe& recipe, PipelineCache& cache,
                   const OutlineMask* outlines = nullptr);

    // The palette a run snaps to: the recipe's colours, an auto palette proposed from the cells'
    // own colours (`count`, `method`, `seed`), or nullopt (each cell keeps its own colour)
    std::optional<RecipePalette> resolvePalette(const Recipe& recipe, const CellSamples& samples);

    // Runs the recipe; nullopt when its grid is Auto and the picture has no grid to find
    std::optional<Outcome> runRecipe(const RgbaImage& image, const Recipe& recipe, PipelineCache* cache = nullptr);
    // Runs the recipe with a grid already resolved. Cells of 1 x 1 (pixel art at its own size) read
    // each pixel as opaque when its alpha is at least `alphaCutoff` (R14); larger cells use
    // `sample.alphaThreshold` per pixel read.
    Outcome runWithSpec(const RgbaImage& image, const Recipe& recipe, const GridSpec& spec,
                        const std::optional<Detection>& detection, PipelineCache* cache = nullptr);

    // "WxH" of the fixed picture before shrinking: what kept specks are recorded against
    std::string gridKeyOf(const Grid& grid);
    // One line on what auto-detection found, for people and agents alike
    std::string describeDetection(const RgbaImage& image, const std::optional<Detection>& detection);

}
