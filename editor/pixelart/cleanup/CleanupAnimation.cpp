// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/animation.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupAnimation.h"

#include "CleanupCommon.h"
#include "CleanupGrid.h"
#include "CleanupLost.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace jasperengine::editor::pixelart::cleanup {

    namespace {

        // Two cell sizes this close (a share) agree (animation.ts)
        constexpr double AGREE = 0.03;
        // background.ts defaultBackgroundOptions.tolerance, which a sheet's own tolerance is moved from
        constexpr double kDefaultTolerance = 0.08;

        std::string animationClearKey(const Recipe& recipe) {
            return std::to_string(recipe.removeBackground) + ":" + std::to_string(recipe.background.tolerance);
        }

        // animation.ts clearedSheet's body, for one picture
        ClearedSheet animationClear(const RgbaImage& image, const Recipe& recipe) {
            if (!recipe.removeBackground) return {image, {}};
            const std::vector<Rgba8> colors = sheetBackground(image);
            // A sheet's tolerance follows how noisy its fill is (JPEG noise), so a dark outline next
            // to a dark background is kept; the recipe's tolerance moves it from there
            const double noise = colors.empty() ? 0.0 : fillNoise(image, colors[0]);
            const double tolerance = std::max(0.02, std::max(0.04, 4 * noise + 0.01) + recipe.background.tolerance - kDefaultTolerance);
            const Unlined unlined = removeGridLines(image, colors, tolerance);
            return {clearSheet(unlined.image, unlined.colors, tolerance).image, colors};
        }

        int animationPartialAlpha(const RgbaImage& image) {
            int partial = 0;
            const auto pixels = image.pixels();
            for (size_t i = 3; i < pixels.size(); i += 4) {
                if (pixels[i] > 0 && pixels[i] < 255) partial++;
            }
            return partial;
        }

        std::optional<double> animationWeightedMedian(std::vector<std::pair<double, double>> entries) {
            std::vector<std::pair<double, double>> sorted;
            for (const auto& e : entries) {
                if (e.second > 0) sorted.push_back(e);
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            double total = 0;
            for (const auto& e : sorted) total += e.second;
            double seen = 0;
            for (const auto& e : sorted) {
                seen += e.second;
                if (seen >= total / 2) return e.first;
            }
            if (sorted.empty()) return std::nullopt;
            return sorted.back().first;
        }

        std::string animationBoxKey(const FrameBox& box) {
            return std::to_string(box.x) + "," + std::to_string(box.y) + "," + std::to_string(box.width) + "," + std::to_string(box.height);
        }

        // Whether two boxes share most of their height: frames on one row of the sheet
        bool animationOverlapsVertically(const FrameBox& a, const FrameBox& b) {
            return std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y) >= 0.5 * std::min(a.height, b.height);
        }

        // One animation's frames on one canvas, lined up by the sheet's anchor
        Layout animationPlace(const std::vector<const FixedFrame*>& included, const Sheet& sheet, double cellHeight) {
            if (included.empty()) return {};
            // On the ground, each frame keeps its height as drawn on the sheet (a hop stays a hop):
            // its top, in cells, above the bottom of its row of boxes; the lowest feet set the ground
            std::vector<int> drawn;
            for (const FixedFrame* frame : included) {
                int bottom = std::numeric_limits<int>::min();
                for (const FixedFrame* other : included) {
                    if (animationOverlapsVertically(other->box, frame->box)) bottom = std::max(bottom, other->box.y + other->box.height);
                }
                drawn.push_back((int)jsRound((std::max(0, frame->box.y) + frame->spec.offsetY - bottom) / cellHeight));
            }
            int ground = std::numeric_limits<int>::min();
            for (size_t i = 0; i < included.size(); i++) ground = std::max(ground, drawn[i] + (included[i]->anchor ? included[i]->anchor->y : 0));
            std::vector<Point> offsets;
            std::vector<PixelArt> arts;
            for (size_t i = 0; i < included.size(); i++) {
                const FixedFrame& frame = *included[i];
                const Point nudge = sheet.frames[frame.index].nudge;
                arts.push_back(frame.art);
                // Frames without an anchor sit as they are in their boxes
                if (!frame.anchor) offsets.push_back(nudge);
                else if (sheet.anchor == Anchor::Ground) offsets.push_back({nudge.x - frame.anchor->x, nudge.y + drawn[i] - ground});
                else offsets.push_back({nudge.x - frame.anchor->x, nudge.y - frame.anchor->y});
            }
            return layOut(arts, offsets, sheet.padding);
        }

        struct FrameInput {
            FrameBox box;
            const AnimationCache::FrameEntry* entry = nullptr;
            PipelineCache* pipeline = nullptr;
            int delayMs = 0;
        };

        // The shared part of runSheet and runFrames: every frame on one cell, one palette, then the clips
        void animationFix(const std::vector<FrameInput>& inputs, const Recipe& recipe, const Sheet& sheet, AnimationOutcome& out) {
            std::vector<std::optional<Detection>> detections;
            for (const FrameInput& input : inputs) detections.push_back(input.entry->detection);
            const std::optional<SharedCell> found = sharedCell(detections);
            const bool given = recipe.grid.kind == GridKind::Cells;
            out.cellWidth = given ? recipe.grid.spec.cellWidth : (found ? found->size : 8);
            out.cellHeight = given ? recipe.grid.spec.cellHeight : (found ? found->size : 8);
            if (!given && found) out.confidence = found->confidence;

            // Each frame alone: no hand edits, no shrinking, no cropping, its background already cleared
            Recipe frameRecipe = recipe;
            frameRecipe.moved.clear();
            frameRecipe.targetWidth.reset();
            frameRecipe.removeBackground = false;
            frameRecipe.crop = false;
            frameRecipe.edits = SizedEdits{};
            frameRecipe.cleanKeep = SizedCells{};

            auto fixAll = [&](const Recipe& r) {
                std::vector<FixedFrame> fixed;
                for (size_t i = 0; i < inputs.size(); i++) {
                    const FrameInput& input = inputs[i];
                    const RgbaImage& crop = input.entry->crop;
                    const GridOffsets offsets = !crop.empty() ? fitOffsets(crop, out.cellWidth, out.cellHeight) : GridOffsets{};
                    FixedFrame frame;
                    frame.index = (int)i;
                    frame.box = input.box;
                    frame.crop = crop;
                    frame.spec = {out.cellWidth, out.cellHeight, offsets.offsetX, offsets.offsetY};
                    frame.detection = input.entry->detection;
                    frame.delayMs = input.delayMs;
                    if (!crop.empty()) {
                        frame.outcome = runWithSpec(crop, r, frame.spec, frame.detection, input.pipeline);
                    } else {
                        frame.outcome.spec = frame.spec;
                        frame.outcome.palette = r.palette;
                    }
                    fixed.push_back(std::move(frame));
                }
                return fixed;
            };

            // One palette for the whole animation: an auto palette is proposed once, from every
            // included frame's own colours (ritmo's proposeForSheet), never per frame
            if (!frameRecipe.palette && frameRecipe.autoPalette) {
                Recipe own = frameRecipe;
                own.autoPalette.reset();
                AnimationOutcome probe;
                probe.sheet = sheet;
                probe.frames = fixAll(own);
                std::vector<PixelArt> arts;
                for (const FixedFrame& f : probe.frames) arts.push_back(f.outcome.art);
                const std::vector<PixelArt> shared = sharePalette(arts);
                for (size_t i = 0; i < probe.frames.size(); i++) probe.frames[i].art = shared[i];
                for (const SheetAnimation& animation : sheet.animations) {
                    ClipOutcome clip;
                    clip.include = animation.include;
                    for (int i : animation.frames) {
                        if (i >= 0 && i < (int)probe.frames.size() && sheet.frames[i].include) clip.placed.push_back({probe.frames[i].art, {}, i});
                    }
                    probe.clips.push_back(std::move(clip));
                }
                const AutoPalette wanted = *frameRecipe.autoPalette;
                RecipePalette palette;
                palette.name = std::string("Auto (") + (wanted.method == ProposeMethod::Dominant ? "dominant" : wanted.method == ProposeMethod::Balanced ? "balanced" : "distinct") +
                               ", " + std::to_string(wanted.count) + ")";
                for (const Proposal& p : proposePalette(sheetColors(probe), wanted.count, wanted.method, recipe.seed)) palette.colors.push_back(p.color);
                if (!palette.colors.empty()) frameRecipe.palette = palette;
                frameRecipe.autoPalette.reset();
            }
            out.palette = frameRecipe.palette;

            std::vector<FixedFrame> fixed = fixAll(frameRecipe);
            std::vector<PixelArt> arts;
            for (const FixedFrame& f : fixed) arts.push_back(f.outcome.art);
            const std::vector<PixelArt> shared = sharePalette(arts);
            for (size_t i = 0; i < fixed.size(); i++) {
                fixed[i].art = shared[i];
                fixed[i].anchor = anchorOf(shared[i], sheet.anchor);
            }
            out.frames = std::move(fixed);

            for (size_t a = 0; a < sheet.animations.size(); a++) {
                const SheetAnimation& animation = sheet.animations[a];
                std::vector<const FixedFrame*> included;
                for (int i : animation.frames) {
                    if (i >= 0 && i < (int)out.frames.size() && sheet.frames[i].include) included.push_back(&out.frames[i]);
                }
                const Layout laid = animationPlace(included, sheet, out.cellHeight);
                ClipOutcome clip;
                clip.animation = (int)a;
                clip.name = animation.name;
                clip.include = animation.include;
                for (size_t i = 0; i < laid.frames.size(); i++) clip.placed.push_back({laid.frames[i].art, laid.frames[i].anchor, included[i]->index});
                clip.width = laid.width;
                clip.height = laid.height;
                out.clips.push_back(std::move(clip));
            }
            std::set<int> exported;
            for (const SheetAnimation& animation : sheet.animations) {
                if (animation.include) exported.insert(animation.frames.begin(), animation.frames.end());
            }
            if (frameRecipe.palette) {
                for (const FixedFrame& frame : out.frames) {
                    if (!exported.count(frame.index) || !sheet.frames[frame.index].include || frame.crop.empty()) continue;
                    for (LostColor lost : lostColors(frame.outcome.samples, frame.outcome.art, recipe.lostThreshold)) {
                        lost.frame = frame.index;
                        out.lost.push_back(std::move(lost));
                    }
                }
            }
            out.sheet = sheet;
        }

    }

    ClearedSheet clearedSheet(const RgbaImage& image, const Recipe& recipe, AnimationCache& cache) {
        const std::string key = animationClearKey(recipe);
        const uint64_t id = image.hash();
        if (!cache.hasCleared || cache.clearedImage != id || cache.clearedKey != key) {
            const ClearedSheet cleared = animationClear(image, recipe);
            cache.hasCleared = true;
            cache.clearedImage = id;
            cache.clearedKey = key;
            cache.cleared = cleared.cleared;
            cache.colors = cleared.colors;
            cache.frames.clear();
        }
        return {cache.cleared, cache.colors};
    }

    std::vector<SheetFrame> detectFrames(const RgbaImage& image, const Recipe& recipe, AnimationCache& cache) {
        std::vector<SheetFrame> frames;
        for (const FrameBox& box : findFrames(clearedSheet(image, recipe, cache).cleared)) frames.push_back(frameOf(box));
        return frames;
    }

    bool isAnimationSheet(const RgbaImage& image, const Recipe& recipe, const std::vector<SheetFrame>& frames, AnimationCache& cache) {
        std::vector<FrameBox> boxes;
        for (const SheetFrame& frame : frames) boxes.push_back(frame.box);
        return looksAnimated(clearedSheet(image, recipe, cache).cleared, boxes);
    }

    std::optional<SharedCell> sharedCell(const std::vector<std::optional<Detection>>& detections) {
        std::vector<std::pair<double, double>> votes;
        for (const auto& detection : detections) {
            if (!detection) continue;
            votes.push_back({detection->x.cell, detection->x.confidence + 0.01});
            votes.push_back({detection->y.cell, detection->y.confidence + 0.01});
        }
        const std::optional<double> size = animationWeightedMedian(votes);
        if (!size) return std::nullopt;
        // Sure when the frames that agree on the size are sure of it
        double sum = 0;
        int agreeing = 0;
        for (const auto& detection : detections) {
            if (detection && std::abs((detection->x.cell + detection->y.cell) / 2 - *size) <= AGREE * *size) {
                sum += detection->confidence;
                agreeing++;
            }
        }
        const double confidence = agreeing == 0 ? 0 : sum / agreeing;
        const double share = double(agreeing) / std::max<size_t>(1, detections.size());
        return SharedCell{jsRound(*size * 1000) / 1000, confidence * share};
    }

    AnimationOutcome runSheet(const RgbaImage& image, const Recipe& recipe, const Sheet& sheet, AnimationCache& cache) {
        const ClearedSheet cleared = clearedSheet(image, recipe, cache);
        std::map<std::string, AnimationCache::FrameEntry> entries;
        for (const SheetFrame& frame : sheet.frames) {
            const std::string key = animationBoxKey(frame.box);
            if (entries.count(key)) continue;
            auto previous = cache.frames.find(key);
            if (previous != cache.frames.end()) {
                entries.emplace(key, std::move(previous->second));
                continue;
            }
            AnimationCache::FrameEntry entry;
            entry.crop = cropImage(cleared.cleared, frame.box);
            if (!entry.crop.empty()) entry.detection = detectGrid(entry.crop);
            entries.emplace(key, std::move(entry));
        }
        cache.frames = std::move(entries);
        std::vector<FrameInput> inputs;
        for (const SheetFrame& frame : sheet.frames) {
            AnimationCache::FrameEntry& entry = cache.frames[animationBoxKey(frame.box)];
            inputs.push_back({frame.box, &entry, &entry.pipeline, 0});
        }
        AnimationOutcome out;
        out.backgroundFound = cleared.colors;
        out.cleared = cleared.cleared;
        out.partialAlpha = animationPartialAlpha(image);
        animationFix(inputs, recipe, sheet, out);
        return out;
    }

    AnimationOutcome runFrames(const std::vector<RgbaImage>& frames, const std::vector<int>& delaysMs, const std::string& name,
                               const Recipe& recipe, const std::optional<Sheet>& given, AnimationCache& cache) {
        Sheet sheet = given.value_or(Sheet{});
        sheet.frames.clear();
        std::vector<int> all;
        for (size_t i = 0; i < frames.size(); i++) {
            sheet.frames.push_back(frameOf({0, 0, frames[i].width(), frames[i].height()}));
            all.push_back((int)i);
        }
        // the frames given keep their own include and nudge flags when a recipe recorded them
        if (given && given->frames.size() == frames.size()) {
            for (size_t i = 0; i < frames.size(); i++) {
                sheet.frames[i].include = given->frames[i].include;
                sheet.frames[i].nudge = given->frames[i].nudge;
            }
        }
        const bool keepAnimations = given && !given->animations.empty() && [&] {
            std::set<int> seen;
            for (const SheetAnimation& a : given->animations) {
                for (int i : a.frames) {
                    if (i < 0 || i >= (int)frames.size() || !seen.insert(i).second) return false;
                }
            }
            return seen.size() == frames.size();
        }();
        if (!keepAnimations) {
            SheetAnimation animation = animationOf(isAnimationName(name) ? name : "animation-1", all);
            sheet.animations = {animation};
        }

        std::map<std::string, AnimationCache::FrameEntry> entries;
        const std::string clearKey = animationClearKey(recipe);
        for (size_t i = 0; i < frames.size(); i++) {
            const std::string key = "frame:" + std::to_string(i) + ":" + std::to_string(frames[i].hash()) + ":" + clearKey;
            auto previous = cache.frames.find(key);
            if (previous != cache.frames.end()) {
                entries.emplace(key, std::move(previous->second));
                continue;
            }
            AnimationCache::FrameEntry entry;
            entry.crop = animationClear(frames[i], recipe).cleared;
            if (!entry.crop.empty()) entry.detection = detectGrid(entry.crop);
            entries.emplace(key, std::move(entry));
        }
        cache.frames = std::move(entries);
        std::vector<FrameInput> inputs;
        AnimationOutcome out;
        for (size_t i = 0; i < frames.size(); i++) {
            const std::string key = "frame:" + std::to_string(i) + ":" + std::to_string(frames[i].hash()) + ":" + clearKey;
            AnimationCache::FrameEntry& entry = cache.frames[key];
            inputs.push_back({sheet.frames[i].box, &entry, &entry.pipeline, i < delaysMs.size() ? delaysMs[i] : 0});
            out.partialAlpha += animationPartialAlpha(frames[i]);
        }
        animationFix(inputs, recipe, sheet, out);
        return out;
    }

    std::vector<Rgba8> sheetColors(const AnimationOutcome& outcome) {
        std::vector<Rgba8> colors;
        for (const ClipOutcome* clip : exportedClips(outcome)) {
            for (const ClipFrame& frame : clip->placed) {
                for (int32_t index : frame.art.indices) {
                    if (index != TRANSPARENT) colors.push_back(frame.art.palette[index]);
                }
            }
        }
        return colors;
    }

    std::vector<const ClipOutcome*> exportedClips(const AnimationOutcome& outcome) {
        std::vector<const ClipOutcome*> clips;
        for (const ClipOutcome& clip : outcome.clips) {
            if (clip.include && !clip.placed.empty()) clips.push_back(&clip);
        }
        return clips;
    }

    std::vector<AtlasClip> atlasClips(const AnimationOutcome& outcome) {
        std::vector<AtlasClip> clips;
        for (const ClipOutcome* clip : exportedClips(outcome)) {
            const SheetAnimation& animation = outcome.sheet.animations[clip->animation];
            AtlasClip atlas;
            atlas.name = clip->name;
            atlas.anchor = clip->placed[0].anchor;
            atlas.fps = animation.fps;
            atlas.loop = animation.loop;
            const int fromFps = (int)jsRound(1000 / std::max(0.1, animation.fps));
            for (const ClipFrame& frame : clip->placed) {
                atlas.frames.push_back(frame.art);
                const int delay = outcome.frames[frame.index].delayMs;
                atlas.durationsMs.push_back(delay > 0 ? delay : fromFps);
            }
            clips.push_back(std::move(atlas));
        }
        return clips;
    }

}
