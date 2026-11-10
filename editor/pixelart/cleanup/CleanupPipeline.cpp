// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/pipeline.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupPipeline.h"

#include "CleanupBackground.h"
#include "CleanupCommon.h"
#include "CleanupEdits.h"
#include "CleanupGrid.h"
#include "CleanupLost.h"
#include "CleanupOutline.h"
#include "CleanupPalette.h"
#include "CleanupReduce.h"
#include "CleanupSample.h"
#include "CleanupSpecks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::editor::pixelart::cleanup {

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace pipeline_ts {

        // The picture's identity for the cache: its size and pixels
        uint64_t imageKey(const RgbaImage& image) { return image.hash(); }

        std::string number(double value) {
            char text[64];
            std::snprintf(text, sizeof(text), "%.17g", value);
            return text;
        }

        std::string sampleKey(const SampleOptions& sample) { return number(sample.inset) + "," + number(sample.alphaThreshold); }

        // ritmo's fmt: rounded to 2 decimals, without trailing zeros
        std::string shortNumber(double value) {
            char text[64];
            std::snprintf(text, sizeof(text), "%.2f", jsRound(value * 100) / 100);
            std::string out = text;
            while (!out.empty() && out.back() == '0') out.pop_back();
            if (!out.empty() && out.back() == '.') out.pop_back();
            return out;
        }

        const char* proposeName(ProposeMethod method) {
            switch (method) {
                case ProposeMethod::Dominant: return "Dominant";
                case ProposeMethod::Balanced: return "Balanced";
                case ProposeMethod::Distinct: return "Distinct";
            }
            return "Dominant";
        }

        int partialAlphaOf(const RgbaImage& image) {
            int partial = 0;
            const std::span<const uint8_t> data = image.pixels();
            for (size_t i = 3; i < data.size(); i += 4) {
                if (data[i] > 0 && data[i] < 255) partial++;
            }
            return partial;
        }

    }

    std::optional<ResolvedGrid> resolveGrid(const RgbaImage& image, const GridChoice& choice) {
        using namespace pipeline_ts;
        switch (choice.kind) {
            case GridKind::Cells:
                return ResolvedGrid{choice.spec, std::nullopt};
            case GridKind::Size: {
                const double width = std::max(1.0, jsRound(choice.width));
                const double height = std::max(1.0, jsRound(choice.height ? *choice.height : (image.height() * width) / image.width()));
                return ResolvedGrid{GridSpec{image.width() / width, image.height() / height, 0, 0}, std::nullopt};
            }
            case GridKind::Auto: {
                const std::optional<Detection> detection = detectGrid(image);
                if (!detection) return std::nullopt;
                return ResolvedGrid{detection->spec, detection};
            }
        }
        return std::nullopt;
    }

    bool outlinesOf(const RgbaImage& image, const GridSpec& spec, const Recipe& recipe, PipelineCache& cache) {
        using namespace pipeline_ts;
        const uint64_t key = imageKey(image);
        if (!cache.hasMask || cache.maskImage != key) {
            cache.mask = outlineMask(image);
            cache.maskImage = key;
            cache.hasMask = true;
        }
        const double cell = (spec.cellWidth + spec.cellHeight) / 2;
        // Pictures at their own size (one pixel per cell) have no strokes to split
        if (recipe.outlines == Outlines::Off || cell < 2) return false;
        return recipe.outlines == Outlines::On || hasOutlines(cache.mask, cell);
    }

    Grid buildGrid(const RgbaImage& image, const GridSpec& spec, const Recipe& recipe, PipelineCache& cache, const OutlineMask* outlines) {
        using namespace pipeline_ts;
        const std::string key = number(spec.cellWidth) + "," + number(spec.cellHeight) + "," + number(spec.offsetX) + "," +
                                number(spec.offsetY) + "," + std::to_string(int(recipe.follow)) + "," + (outlines ? "1" : "0");
        const uint64_t image64 = imageKey(image);
        if (!cache.hasGrid || cache.gridImage != image64 || cache.gridKey != key) {
            Grid next = uniformGrid(image.width(), image.height(), spec);
            if (recipe.follow != Follow::Off) {
                RefineOptions options;
                options.bend = recipe.follow == Follow::Bend;
                options.outlines = outlines;
                next = refineGrid(image, next, options);
            }
            cache.baseGrid = std::move(next);
            cache.gridImage = image64;
            cache.gridKey = key;
            cache.hasGrid = true;
        }
        if (recipe.moved.empty()) return cache.baseGrid;
        Grid grid = cache.baseGrid;
        for (const auto& [id, position] : recipe.moved) {
            const size_t colon = id.find(':');
            if (colon == std::string::npos) continue;
            const std::string axis = id.substr(0, colon);
            char* end = nullptr;
            const long index = std::strtol(id.c_str() + colon + 1, &end, 10);
            if (end == id.c_str() + colon + 1 || *end != '\0') continue;
            std::vector<double>& cuts = axis == "x" ? grid.xs : grid.ys;
            if (index < 0 || index >= long(cuts.size())) continue;
            // A bent line dragged by hand moves as a whole
            if (grid.bend) {
                std::vector<std::vector<double>>& lines = axis == "x" ? grid.bend->xs : grid.bend->ys;
                if (size_t(index) < lines.size()) {
                    for (double& point : lines[size_t(index)]) point += position - cuts[size_t(index)];
                }
            }
            cuts[size_t(index)] = position;
        }
        return grid;
    }

    std::optional<RecipePalette> resolvePalette(const Recipe& recipe, const CellSamples& samples) {
        using namespace pipeline_ts;
        if (recipe.palette && !recipe.palette->colors.empty()) return recipe.palette;
        if (!recipe.autoPalette) return std::nullopt;
        const std::vector<Proposal> proposals =
            proposePalette(cellColors(samples), std::clamp(recipe.autoPalette->count, 2, 256), recipe.autoPalette->method, recipe.seed);
        if (proposals.empty()) return std::nullopt;
        RecipePalette palette;
        palette.name = std::string("Auto (") + proposeName(recipe.autoPalette->method) + ")";
        for (const Proposal& proposal : proposals) palette.colors.push_back(proposal.color);
        return palette;
    }

    std::optional<Outcome> runRecipe(const RgbaImage& image, const Recipe& recipe, PipelineCache* cache) {
        using namespace pipeline_ts;
        const std::optional<ResolvedGrid> resolved = resolveGrid(image, recipe.grid);
        if (!resolved) return std::nullopt;
        return runWithSpec(image, recipe, resolved->spec, resolved->detection, cache);
    }

    Outcome runWithSpec(const RgbaImage& image, const Recipe& recipe, const GridSpec& spec, const std::optional<Detection>& detection,
                        PipelineCache* cache) {
        using namespace pipeline_ts;
        PipelineCache local;
        PipelineCache& use = cache ? *cache : local;
        Outcome outcome;
        outcome.spec = spec;
        outcome.detection = detection;
        outcome.partialAlpha = partialAlphaOf(image);

        // 1-3: outline mask, lines
        outcome.outlines = outlinesOf(image, spec, recipe, use);
        outcome.grid = buildGrid(image, spec, recipe, use, outcome.outlines ? &use.mask : nullptr);

        // 4: sampling. One pixel per cell is art at its own size: R14's cutoff decides each pixel.
        SampleOptions sample = recipe.sample;
        if (spec.cellWidth == 1 && spec.cellHeight == 1) sample.alphaThreshold = recipe.alphaCutoff;
        const std::string key = sampleKey(sample);
        const uint64_t picture = imageKey(image);
        if (!use.hasSamples || use.samplesImage != picture || use.samplesGrid != outcome.grid || use.samplesKey != key) {
            use.samples = sampleCells(image, outcome.grid, sample);
            use.samplesImage = picture;
            use.samplesGrid = outcome.grid;
            use.samplesKey = key;
            use.hasSamples = true;
            use.hasSnap = use.hasOwn = false;
        }
        outcome.samples = use.samples;
        const CellSamples& samples = outcome.samples;

        // 5-6: snap, thin outlines
        outcome.palette = resolvePalette(recipe, samples);
        const std::vector<Rgba8> palette = outcome.palette ? outcome.palette->colors : std::vector<Rgba8>();
        QuantizeOptions snap;
        snap.palette = palette;
        snap.metric = recipe.metric;
        snap.dither = recipe.dither;
        snap.ditherStrength = recipe.ditherStrength;
        std::string snapKey = std::to_string(int(recipe.metric)) + "/" + std::to_string(int(recipe.dither)) + "/" + std::to_string(recipe.ditherStrength) + "/";
        for (const Rgba8& color : palette) snapKey += std::to_string(jasperengine::pixelart::packRgb(color)) + ",";
        if (!use.hasSnap || use.snapKey != snapKey) {
            use.snapped = quantize(samples, snap);
            use.snapKey = snapKey;
            use.hasSnap = true;
        }
        PixelArt art = use.snapped;
        if (outcome.outlines) art = thinOutlines(art, outcome.grid, use.mask);

        // 7: each cell's own colour, where the background is told apart from the subject; lost colours
        PixelArt own = art;
        if (!palette.empty()) {
            QuantizeOptions plain;
            plain.metric = recipe.metric;
            const std::string ownKey = std::to_string(int(recipe.metric));
            if (!use.hasOwn || use.ownKey != ownKey) {
                use.own = quantize(samples, plain);
                use.ownKey = ownKey;
                use.hasOwn = true;
            }
            own = use.own;
            std::vector<uint8_t> skip;
            if (recipe.removeBackground) skip = findBackground(own, backgroundColors(own), recipe.background);
            outcome.lost = lostColors(samples, art, recipe.lostThreshold, recipe.removeBackground ? &skip : nullptr, 2, &own);
        }

        // 8: specks
        const std::string gridKey = std::to_string(art.width) + "x" + std::to_string(art.height);
        outcome.cleanKeepStale = recipe.cleanKeep.size != gridKey && !recipe.cleanKeep.cells.empty();
        if (recipe.cleanup.size > 0) {
            CleanupOptions options;
            options.size = recipe.cleanup.size;
            options.strength = recipe.cleanup.strength;
            options.metric = recipe.metric;
            if (outcome.palette) options.protect = outcome.palette->locked;
            if (recipe.cleanKeep.size == gridKey) options.keep = recipe.cleanKeep.cells;
            Cleaned cleaned = cleanUp(art, samples, options);
            art = std::move(cleaned.art);
            outcome.specks = std::move(cleaned.specks);
        }

        // 9: shrink
        PixelArt ownShrunk = own;
        if (recipe.targetWidth && *recipe.targetWidth < art.width) {
            const double width = std::max(1.0, jsRound(*recipe.targetWidth));
            const double height = std::max(1.0, jsRound((art.height * width) / art.width));
            art = reduce(art, width, height, recipe.reduceMode);
            if (recipe.removeBackground) ownShrunk = reduce(own, width, height, recipe.reduceMode);
        }

        // 10: background
        const std::string size = std::to_string(art.width) + "x" + std::to_string(art.height);
        outcome.editsStale = recipe.edits.size != size && !recipe.edits.strokes.empty();
        std::vector<uint8_t> mask;
        bool hasMask = false;
        if (recipe.removeBackground) {
            const std::vector<int> colors = backgroundColors(ownShrunk);
            for (int index : colors) outcome.backgroundFound.push_back(ownShrunk.palette[size_t(index)]);
            mask = findBackground(ownShrunk, colors, recipe.background);
            hasMask = true;
        }

        // 11: hand edits
        if (!outcome.editsStale && !recipe.edits.strokes.empty()) {
            std::vector<Edit> edits;
            for (const std::vector<Edit>& stroke : recipe.edits.strokes) edits.insert(edits.end(), stroke.begin(), stroke.end());
            if (!hasMask) mask.assign(size_t(art.width) * size_t(art.height), 0);
            Edited edited = applyEdits(art, mask, edits, recipe.metric);
            art = std::move(edited.art);
            mask = std::move(edited.mask);
            outcome.painted = std::move(edited.painted);
            hasMask = true;
        }
        if (hasMask) art = clearMasked(art, mask);

        // 12: the alpha cutoff (R14). Cells are already either a palette colour or transparent, so
        // the output (toRgba) is only fully opaque or fully transparent pixels; the source's partial
        // pixels were decided at sampling and are counted in partialAlpha.

        // 13: crop
        outcome.exported = recipe.crop ? cropToContent(art) : art;
        outcome.art = std::move(art);
        return outcome;
    }

    std::string gridKeyOf(const Grid& grid) {
        using namespace pipeline_ts;
        const GridSize size = gridSize(grid);
        return std::to_string(size.width) + "x" + std::to_string(size.height);
    }

    std::string describeDetection(const RgbaImage& image, const std::optional<Detection>& detection) {
        using namespace pipeline_ts;
        if (!detection) return "No grid found: the picture has no edges. Set the cell size by hand.";
        const int percent = int(jsRound(detection->confidence * 100));
        const GridSize size = gridSize(uniformGrid(image.width(), image.height(), detection->spec));
        const char* verdict = detection->confidence >= 0.5   ? "Looks right."
                              : detection->confidence >= 0.3 ? "Probably right: check the grid on the picture."
                                                             : "Unsure: check the grid, and set the cell size by hand if it is off.";
        return "Found " + shortNumber(detection->spec.cellWidth) + " x " + shortNumber(detection->spec.cellHeight) + " px cells, making " +
               std::to_string(size.width) + " x " + std::to_string(size.height) + " pixels. Confidence " + std::to_string(percent) + "%. " + verdict;
    }

}
