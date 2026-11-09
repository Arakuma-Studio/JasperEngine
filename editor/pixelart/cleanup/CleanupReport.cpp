// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/report.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupReport.h"

#include "CleanupCommon.h"
#include "CleanupGrid.h"
#include "CleanupRecipe.h"
#include "CleanupSample.h"
#include "CleanupSheetOutput.h"
#include "pixelart/Palette.h"
#include "pixelart/lint/EffectLints.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;
    using jasperengine::pixelart::Palette;
    using nlohmann::json;

    namespace {

        // Two colours this close (OKLab) count as the same when measuring the fit (report.ts)
        constexpr double FIT_DISTANCE = 0.06;
        // Pixel art at its own size has few colours; a blurry or smoothed picture has many (report.ts)
        constexpr size_t FEW_COLORS = 256;
        // ritmo's lost colour reports stop at this many cell locations (R13)
        constexpr size_t LOST_SHOWN = 20;

        double reportRound(double value, int places = 3) {
            const double scale = std::pow(10.0, places);
            return jsRound(value * scale) / scale;
        }

        std::string reportFixed(double value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", reportRound(value));
            return text;
        }

        std::string reportPercent(double value) { return std::to_string((int)jsRound(value * 100)) + "%"; }

        std::string reportPlural(int count, const char* word) { return std::to_string(count) + " " + word + (count == 1 ? "" : "s"); }

        std::string reportSize(int width, int height) { return std::to_string(width) + "\xC3\x97" + std::to_string(height); }

        // the FAILs a fix removes: for art at its own size, the verdict stays fixable with these (R29)
        bool reportRemovable(const std::string& id) {
            return id == "off_palette" || id == "partial_alpha" || id == "transparent_background" || id == "colors";
        }

        Palette reportLintPalette(const RecipePalette& recipe) {
            Palette palette;
            palette.name = recipe.name;
            palette.colors = recipe.colors;
            return palette;
        }

        lint::LintInput reportLintInput(const ReportOptions& options, const Palette* palette) {
            lint::LintInput input;
            input.palette = palette;
            input.backdrops = options.backdrops;
            input.backdropsSource = options.backdropsSource;
            input.rules = options.rules;
            return input;
        }

        lint::LintOptions reportLintOptions(const ReportOptions& options) {
            lint::LintOptions out;
            out.only = options.only;
            out.thresholds = options.thresholds;
            return out;
        }

        // the guards a FAIL lint trips; `removable` FAILs are left to the fix when asked
        void reportFailGuards(const lint::LintReport& lints, bool skipRemovable, std::vector<Guard>& guards) {
            for (const lint::LintResult& r : lints.results) {
                if (r.status != lint::LintStatus::Fail) continue;
                if (skipRemovable && reportRemovable(r.id)) continue;
                guards.push_back({r.id, r.measured, r.threshold});
            }
        }

        int reportOpaqueCells(const CellSamples& samples) {
            int n = 0;
            for (const Cell& cell : samples.cells) {
                if (cell.colors.size() > static_cast<size_t>(cell.transparent)) n++;
            }
            return n;
        }

        void reportLostShare(const std::vector<LostColor>& lost, int opaqueCells, std::vector<Guard>& guards) {
            if (opaqueCells <= 0) return;
            size_t cells = 0;
            for (const LostColor& entry : lost) cells += entry.cells.size();
            const double share = double(cells) / opaqueCells;
            if (share > LOST_SHARE) guards.push_back({"lost_share", share, LOST_SHARE});
        }

        ReportLost reportLostEntry(const LostColor& entry, int gridWidth) {
            ReportLost out;
            out.color = entry.color;
            out.became = entry.became;
            out.pixels = (int)entry.cells.size();
            out.frame = entry.frame < 0 ? -1 : entry.frame + 1;
            for (size_t i = 0; i < entry.cells.size() && i < LOST_SHOWN; i++) {
                out.at.push_back({entry.cells[i] % gridWidth, entry.cells[i] / gridWidth});
            }
            return out;
        }

        void reportReplacePartialAlpha(lint::LintReport& lints, const RgbaImage& source) {
            for (lint::LintResult& r : lints.results) {
                if (r.id == "partial_alpha") {
                    const std::vector<PixelImage> frames = {source};
                    r = lint::lintPartialAlpha(lint::frameView(frames));
                    break;
                }
            }
            lints.recount();
        }

        // Frames on one canvas (lint() takes one size); smaller clips stay top-left
        std::vector<PixelImage> reportCommonFrames(const std::vector<AtlasClip>& clips) {
            int width = 0, height = 0;
            for (const AtlasClip& clip : clips) {
                for (const PixelArt& art : clip.frames) {
                    width = std::max(width, art.width);
                    height = std::max(height, art.height);
                }
            }
            std::vector<PixelImage> frames;
            for (const AtlasClip& clip : clips) {
                for (const PixelArt& art : clip.frames) {
                    PixelImage frame(width, height);
                    frame.blit(toRgba(art), 0, 0);
                    frames.push_back(std::move(frame));
                }
            }
            return frames;
        }

    }

    double gridFit(const CellSamples& samples) {
        std::unordered_map<uint32_t, Lab> labs;
        const auto lab = [&](uint32_t packed) -> const Lab& {
            auto it = labs.find(packed);
            if (it == labs.end()) it = labs.emplace(packed, oklabOf(rgbOf(packed))).first;
            return it->second;
        };
        double near = 0, total = 0;
        for (const Cell& cell : samples.cells) {
            if (cell.colors.empty()) continue;
            const Lab center = lab(jasperengine::pixelart::packRgb(medianColor(cell.colors)));
            for (uint32_t color : cell.colors) {
                if (labDistance(lab(color), center) <= FIT_DISTANCE) near++;
                total++;
            }
        }
        return total == 0 ? 1 : near / total;
    }

    bool isNative(const RgbaImage& image, const std::optional<Outcome>& outcome) {
        if (!outcome) return true;
        if (std::min(outcome->spec.cellWidth, outcome->spec.cellHeight) < 1.5) return true;
        const std::optional<Detection>& detection = outcome->detection;
        if (detection && detection->confidence < 0.3) return true;
        const bool sure = (!detection || detection->confidence >= CONFIDENT_CONFIDENCE) && gridFit(outcome->samples) >= CONFIDENT_FIT;
        return !sure && artOfImage(image).art.palette.size() <= FEW_COLORS;
    }

    Report reportOfArt(const PixelArt& art, const ReportOptions& options, const RgbaImage* source, const std::optional<RecipePalette>& palette) {
        const std::vector<PixelImage> frames = {toRgba(art)};
        Palette lintPalette;
        if (palette) lintPalette = reportLintPalette(*palette);
        lint::LintInput input = reportLintInput(options, palette ? &lintPalette : nullptr);
        input.frames = frames;
        Report report;
        report.lints = lint::lint(input, reportLintOptions(options));
        if (source) reportReplacePartialAlpha(report.lints, *source);
        int partial = 0;
        if (source) {
            const auto pixels = source->pixels();
            for (size_t i = 3; i < pixels.size(); i += 4) {
                if (pixels[i] > 0 && pixels[i] < 255) partial++;
            }
        }
        report.partialAlpha = partial;
        const int fails = report.lints.fail;
        const int colors = (int)usedColors(art).size();
        reportFailGuards(report.lints, true, report.guards);
        if (fails == 0) report.verdict = options.failOnWarn && report.lints.warn > 0 ? Verdict::Fixable : Verdict::Clean;
        else report.verdict = report.guards.empty() ? Verdict::Fixable : Verdict::NeedsPerson;
        report.summary = fails == 0 ? "Clean pixel art, " + reportSize(art.width, art.height) + ", " + reportPlural(colors, "color") + "."
                                    : "Pixel art at its own size, " + reportSize(art.width, art.height) + ", with " + reportPlural(fails, "rule") + " broken: see lints.";
        report.kind = "pixel-art";
        report.confidence = 1;
        report.sourceWidth = source ? source->width() : art.width;
        report.sourceHeight = source ? source->height() : art.height;
        report.outputWidth = art.width;
        report.outputHeight = art.height;
        report.colors = colors;
        if (palette) {
            report.hasPalette = true;
            report.paletteName = palette->name;
        }
        return report;
    }

    Report reportOfFix(const RgbaImage& image, const Recipe& recipe, const Outcome& outcome, const ReportOptions& options) {
        Report report;
        const double fit = gridFit(outcome.samples);
        // a grid given by hand has no detection to doubt it
        const bool detected = outcome.detection.has_value();
        const double confidence = detected ? outcome.detection->confidence : 1.0;
        ReportOptions lintOptions = options;
        // a removed background must really be gone (R26: default on with remove_background)
        lintOptions.rules.transparentBackground = lintOptions.rules.transparentBackground || recipe.removeBackground;
        Palette lintPalette;
        const std::optional<RecipePalette>& palette = outcome.palette ? outcome.palette : recipe.palette;
        if (palette) lintPalette = reportLintPalette(*palette);
        const std::vector<PixelImage> frames = {toRgba(outcome.exported)};
        lint::LintInput input = reportLintInput(lintOptions, palette ? &lintPalette : nullptr);
        input.frames = frames;
        report.lints = lint::lint(input, reportLintOptions(lintOptions));
        const GridSpec& spec = outcome.spec;
        // A grid given by hand or by size is trusted as given; only the fit can doubt it
        const bool sure = (!detected || confidence >= CONFIDENT_CONFIDENCE) && fit >= CONFIDENT_FIT;
        // Pixelating a picture that is not pixel art has no true grid to fit
        const bool pixelating = recipe.grid.kind == GridKind::Size;
        if (!pixelating) {
            if (detected && confidence < CONFIDENT_CONFIDENCE) report.guards.push_back({"low_confidence", confidence, CONFIDENT_CONFIDENCE});
            if (fit < CONFIDENT_FIT) report.guards.push_back({"low_fit", fit, CONFIDENT_FIT});
        }
        reportFailGuards(report.lints, false, report.guards);
        reportLostShare(outcome.lost, reportOpaqueCells(outcome.samples), report.guards);
        report.verdict = report.guards.empty() ? Verdict::Fixable : Verdict::NeedsPerson;
        report.confidence = pixelating ? fit : std::min(confidence, fit);
        const int colors = (int)usedColors(outcome.exported).size();
        const std::string size = reportSize(outcome.exported.width, outcome.exported.height);
        const std::string fitText = "fit " + reportPercent(fit) + (detected ? ", confidence " + reportPercent(confidence) : "");
        if (pixelating) {
            report.summary = "Pixelated to " + size + ", " + reportPlural(colors, "color") + "." +
                             (report.lints.fail > 0 ? " Some rules are still broken: see lints." : " Look at the preview: whether it reads well is a judgement call.");
        } else if (report.verdict == Verdict::Fixable) {
            report.summary = "Upscaled pixel art: fixes to " + size + ", " + reportPlural(colors, "color") + " (grid " + fitText + ").";
        } else if (sure) {
            report.summary = "Fixes to " + size + ", but it needs a person: see guards.";
        } else {
            report.summary = "Unsure of the grid (" + fitText + "): look at the preview, or set the grid by hand.";
        }
        report.kind = "upscaled";
        report.sourceWidth = image.width();
        report.sourceHeight = image.height();
        ReportGrid grid;
        grid.cellWidth = reportRound(spec.cellWidth);
        grid.cellHeight = reportRound(spec.cellHeight);
        grid.offsetX = reportRound(spec.offsetX, 2);
        grid.offsetY = reportRound(spec.offsetY, 2);
        if (detected) grid.confidence = reportRound(confidence);
        grid.fit = reportRound(fit);
        grid.outlines = outcome.outlines;
        report.grid = grid;
        report.outputWidth = outcome.exported.width;
        report.outputHeight = outcome.exported.height;
        report.colors = colors;
        report.partialAlpha = outcome.partialAlpha;
        if (palette) {
            report.hasPalette = true;
            report.paletteName = palette->name;
            for (const LostColor& entry : outcome.lost) report.lost.push_back(reportLostEntry(entry, outcome.samples.width));
        }
        return report;
    }

    Report reportOfSheet(const RgbaImage& image, const Recipe& recipe, const AnimationOutcome& outcome, const ReportOptions& options) {
        Report report;
        const Sheet& sheet = outcome.sheet;
        const std::vector<AtlasClip> clips = atlasClips(outcome);
        std::vector<PixelImage> frames = reportCommonFrames(clips);
        ReportOptions lintOptions = options;
        lintOptions.rules.sizes.clear();   // judged per animation below: the frames here share a padded canvas
        lintOptions.rules.transparentBackground = lintOptions.rules.transparentBackground || recipe.removeBackground;
        Palette lintPalette;
        const std::optional<RecipePalette>& palette = outcome.palette ? outcome.palette : recipe.palette;
        if (palette) lintPalette = reportLintPalette(*palette);
        lint::LintInput input = reportLintInput(lintOptions, palette ? &lintPalette : nullptr);
        input.frames = frames;
        input.sheet = true;
        input.loop = false;   // a flicker wrap across animations means nothing here; each clip is judged below
        int first = 0;
        std::vector<int> clipStart;
        for (const AtlasClip& clip : clips) {
            lint::LintAnimation animation;
            animation.name = clip.name;
            clipStart.push_back(first);
            for (size_t i = 0; i < clip.frames.size(); i++) animation.frames.push_back(first + (int)i);
            first += (int)clip.frames.size();
            input.animations.push_back(std::move(animation));
        }
        const double shared = (outcome.cellWidth + outcome.cellHeight) / 2;
        input.sharedCell = shared;
        for (const ClipOutcome* clip : exportedClips(outcome)) {
            for (const ClipFrame& placed : clip->placed) {
                const std::optional<Detection>& own = outcome.frames[placed.index].detection;
                input.frameCells.push_back(own && own->confidence >= CONFIDENT_CONFIDENCE ? (own->spec.cellWidth + own->spec.cellHeight) / 2 : 0.0);
            }
        }
        report.lints = lint::lint(input, reportLintOptions(lintOptions));

        // flicker and size belong to each animation, whose frames are a loop and share a size
        bool wantFlicker = options.only.empty() || std::find(options.only.begin(), options.only.end(), "flicker") != options.only.end();
        bool wantSize = options.only.empty() || std::find(options.only.begin(), options.only.end(), "size") != options.only.end();
        for (lint::LintResult& r : report.lints.results) {
            if (r.id == "flicker" && wantFlicker && !clips.empty()) {
                const auto threshold = options.thresholds.find("flicker");
                bool haveResult = false;
                lint::LintResult worst;
                for (size_t c = 0; c < clips.size(); c++) {
                    std::vector<PixelImage> own;
                    for (const PixelArt& art : clips[c].frames) own.push_back(toRgba(art));
                    lint::LintResult one = lint::lintFlicker(lint::frameView(own), clips[c].loop == Loop::Forward, threshold != options.thresholds.end() ? threshold->second : 0.06);
                    for (lint::LintSpot& spot : one.at) spot.frame += clipStart[c];
                    if (threshold != options.thresholds.end()) one.thresholdSource = "options.flicker";
                    const bool better = one.status != lint::LintStatus::Skip && (!haveResult || one.measured > worst.measured);
                    if (better || (!haveResult && c + 1 == clips.size())) {
                        worst = std::move(one);
                        haveResult = true;
                    }
                }
                if (haveResult) r = std::move(worst);
            }
            if (r.id == "size" && wantSize && !options.rules.sizes.empty()) {
                for (const AtlasClip& clip : clips) {
                    const std::vector<PixelImage> own = {toRgba(clip.frames[0])};
                    lint::LintInput one;
                    one.frames = own;
                    one.rules.sizes = options.rules.sizes;
                    lint::LintOptions onlySize;
                    onlySize.only = {"size"};
                    lint::LintResult result = lint::lint(one, onlySize).results[0];
                    if (result.status == lint::LintStatus::Fail) {
                        result.message = clip.name + ": " + result.message;
                        r = std::move(result);
                        break;
                    }
                    r = std::move(result);
                }
            }
        }
        if (clips.empty()) {
            for (lint::LintResult& r : report.lints.results) {
                if (r.id != "one_frame") continue;
                r.status = lint::LintStatus::Warn;
                r.count = 0;
                r.message = "No animation is exported: check the frames found, add boxes by hand, or put an animation back in";
            }
        }
        report.lints.recount();

        // the frames: an empty one trips a guard, so a picture with a lost frame is never "fixed"
        std::set<int> exported;
        for (const SheetAnimation& animation : sheet.animations) {
            if (animation.include) exported.insert(animation.frames.begin(), animation.frames.end());
        }
        int opaqueCells = 0;
        int emptyFrames = 0;
        for (const FixedFrame& frame : outcome.frames) {
            if (!exported.count(frame.index) || frame.index >= (int)sheet.frames.size() || !sheet.frames[frame.index].include) continue;
            opaqueCells += reportOpaqueCells(frame.outcome.samples);
            const bool drawn = std::any_of(frame.art.indices.begin(), frame.art.indices.end(), [](int32_t index) { return index >= 0; });
            if (!drawn) emptyFrames++;
        }
        if (emptyFrames > 0) report.guards.push_back({"empty_frame", (double)emptyFrames, 0});
        double fitSum = 0;
        for (const FixedFrame& frame : outcome.frames) fitSum += gridFit(frame.outcome.samples);
        const double fit = outcome.frames.empty() ? 0 : fitSum / outcome.frames.size();
        if (outcome.confidence && *outcome.confidence < CONFIDENT_CONFIDENCE) report.guards.push_back({"shared_cell", *outcome.confidence, CONFIDENT_CONFIDENCE});
        std::vector<Guard> failing;
        reportFailGuards(report.lints, false, failing);
        for (Guard& guard : failing) {
            if (guard.id != "empty_frame") report.guards.push_back(std::move(guard));
        }
        reportLostShare(outcome.lost, opaqueCells, report.guards);
        report.verdict = report.guards.empty() ? Verdict::Fixable : Verdict::NeedsPerson;
        report.confidence = std::min(outcome.confidence.value_or(1.0), fit);

        const Atlas strip = pngSheet(clips);
        const int colors = (int)usedColors(strip.art).size();
        int count = 0;
        for (const AtlasClip& clip : clips) count += (int)clip.frames.size();
        const std::string what = reportPlural((int)clips.size(), "animation") + " of " + reportPlural(count, "frame");
        const bool sure = !outcome.confidence || *outcome.confidence >= CONFIDENT_CONFIDENCE;
        if (report.verdict == Verdict::Fixable) {
            report.summary = what + ", " + reportPlural(colors, "color") + " (cells " + reportFixed(shared) + " px" +
                             (outcome.confidence ? ", confidence " + reportPercent(*outcome.confidence) : "") + ").";
        } else if (!sure) {
            report.summary = "Unsure of the grid (confidence " + reportPercent(outcome.confidence.value_or(0)) + "): look at the preview, or set the cell size by hand.";
        } else {
            report.summary = what + ", but it needs a person: see guards.";
        }
        report.kind = "animation";
        report.sourceWidth = image.width();
        report.sourceHeight = image.height();
        ReportGrid grid;
        grid.cellWidth = reportRound(outcome.cellWidth);
        grid.cellHeight = reportRound(outcome.cellHeight);
        if (outcome.confidence) grid.confidence = reportRound(*outcome.confidence);
        grid.fit = reportRound(fit);
        report.grid = grid;
        // The sheet written: every exported animation, each from a new row of frames
        report.outputWidth = strip.art.width;
        report.outputHeight = strip.art.height;
        report.colors = colors;
        report.partialAlpha = outcome.partialAlpha;
        ReportAnimation animation;
        animation.frames = (int)outcome.frames.size();
        animation.included = count;
        animation.anchor = sheet.anchor;
        for (size_t a = 0; a < sheet.animations.size(); a++) {
            const SheetAnimation& source = sheet.animations[a];
            ReportClip clip;
            clip.name = source.name;
            for (int i : source.frames) clip.frames.push_back(i + 1);
            clip.include = source.include;
            clip.fps = source.fps;
            clip.loop = source.loop;
            if (a < outcome.clips.size()) {
                clip.width = outcome.clips[a].width;
                clip.height = outcome.clips[a].height;
            }
            animation.clips.push_back(std::move(clip));
        }
        for (const FixedFrame& frame : outcome.frames) {
            ReportBox box;
            box.frame = frame.index + 1;
            box.box = frame.box;
            box.include = frame.index < (int)sheet.frames.size() ? sheet.frames[frame.index].include : true;
            if (frame.detection) {
                box.cell = reportRound((frame.detection->spec.cellWidth + frame.detection->spec.cellHeight) / 2);
                box.confidence = reportRound(frame.detection->confidence);
            }
            animation.boxes.push_back(std::move(box));
        }
        report.animation = std::move(animation);
        if (palette) {
            report.hasPalette = true;
            report.paletteName = palette->name;
            for (const LostColor& entry : outcome.lost) {
                const int gridWidth = entry.frame >= 0 && entry.frame < (int)outcome.frames.size() ? outcome.frames[entry.frame].outcome.samples.width : 1;
                report.lost.push_back(reportLostEntry(entry, gridWidth));
            }
        }
        return report;
    }

    Checked checkImage(const RgbaImage& image, const Recipe& recipe, const ReportOptions& options) {
        Checked checked;
        std::optional<Outcome> found = runRecipe(image, recipe);
        const ArtOfImage own = artOfImage(image, (int)recipe.alphaCutoff);
        // A picture of many colours is never "already pixel art", whatever its grid says (D6: a
        // photo must not read as clean)
        const bool fewColors = own.art.palette.size() <= FEW_COLORS;
        if (recipe.grid.kind == GridKind::Auto && fewColors && isNative(image, found)) {
            // Fixing pixel art at its own size reads one pixel per cell
            Recipe resolved = recipe;
            resolved.grid.kind = GridKind::Cells;
            resolved.grid.spec = {1, 1, 0, 0};
            resolved.sample.inset = 0;
            checked.outcome = runRecipe(image, resolved);
            const std::optional<RecipePalette>& palette = checked.outcome && checked.outcome->palette ? checked.outcome->palette : recipe.palette;
            checked.report = reportOfArt(own.art, options, &image, palette);
            if (checked.outcome) {
                reportLostShare(checked.outcome->lost, reportOpaqueCells(checked.outcome->samples), checked.report.guards);
                if (!checked.report.guards.empty()) checked.report.verdict = Verdict::NeedsPerson;
                for (const LostColor& entry : checked.outcome->lost) checked.report.lost.push_back(reportLostEntry(entry, checked.outcome->samples.width));
                if (checked.outcome->palette) {
                    resolved.paletteMode = PaletteMode::Fixed;
                    resolved.palette = checked.outcome->palette;
                    resolved.autoPalette.reset();
                }
            }
            checked.recipe = std::move(resolved);
            return checked;
        }
        if (!found) {
            Report report;
            report.verdict = Verdict::NeedsPerson;
            report.summary = "No grid found and the picture is not pixel art at its own size: set the grid by hand.";
            report.kind = "upscaled";
            report.sourceWidth = image.width();
            report.sourceHeight = image.height();
            report.guards.push_back({"no_grid", 0, 1});
            checked.report = std::move(report);
            checked.recipe = recipe;
            return checked;
        }
        Recipe resolved = recipe;
        resolved.grid.kind = GridKind::Cells;
        resolved.grid.spec = found->spec;
        if (found->palette) {
            resolved.paletteMode = PaletteMode::Fixed;
            resolved.palette = found->palette;
            resolved.autoPalette.reset();
        }
        checked.report = reportOfFix(image, recipe, *found, options);
        checked.recipe = std::move(resolved);
        checked.outcome = std::move(found);
        return checked;
    }

    Comparison compareReports(const Report& a, const Report& b) {
        // clean is the best, needs_person the worst (Verdict's own order)
        if (a.verdict != b.verdict) {
            const bool first = a.verdict < b.verdict;
            const Report& better = first ? a : b;
            const Report& worse = first ? b : a;
            return {first ? 'a' : 'b', std::string(first ? "a" : "b") + " is " + verdictName(better.verdict) + ", the other " + verdictName(worse.verdict)};
        }
        const auto count = [](const Report& report, lint::LintStatus status) {
            int sum = 0;
            for (const lint::LintResult& r : report.lints.results) {
                if (r.status == status) sum += std::max(1, r.count);
            }
            return sum;
        };
        for (const auto& [status, name] : {std::pair{lint::LintStatus::Fail, "FAIL"}, std::pair{lint::LintStatus::Warn, "WARN"}}) {
            const int left = count(a, status), right = count(b, status);
            if (left != right) {
                const char* winner = left < right ? "a" : "b";
                return {*winner, std::string(winner) + " has fewer " + name + "s (" + std::to_string(std::min(left, right)) + " against " + std::to_string(std::max(left, right)) + ")"};
            }
        }
        const double fitA = a.grid ? a.grid->fit : 1, fitB = b.grid ? b.grid->fit : 1;
        if (std::abs(fitA - fitB) > 0.01) {
            const char* winner = fitA > fitB ? "a" : "b";
            return {*winner, std::string(winner) + " has the better grid fit"};
        }
        return {'=', "both are equally clean"};
    }

    json reportToJson(const Report& report) {
        json guards = json::array();
        for (const Guard& guard : report.guards) guards.push_back({{"id", guard.id}, {"value", reportRound(guard.value, 4)}, {"threshold", guard.threshold}});
        json out = {{"verdict", verdictName(report.verdict)},
                    {"summary", report.summary},
                    {"kind", report.kind},
                    {"confidence", reportRound(report.confidence)},
                    {"guards", std::move(guards)},
                    {"source", {{"width", report.sourceWidth}, {"height", report.sourceHeight}}},
                    {"output", {{"width", report.outputWidth}, {"height", report.outputHeight}, {"colors", report.colors}}},
                    {"lints", lint::toJson(report.lints)},
                    {"partial_alpha", report.partialAlpha}};
        if (report.grid) {
            const ReportGrid& g = *report.grid;
            json grid = {{"cell_width", g.cellWidth}, {"cell_height", g.cellHeight}, {"offset_x", g.offsetX}, {"offset_y", g.offsetY}, {"fit", g.fit}};
            if (g.confidence) grid["confidence"] = *g.confidence;
            if (g.outlines) grid["outlines"] = *g.outlines;
            out["grid"] = std::move(grid);
        }
        if (report.hasPalette) {
            json lost = json::array();
            for (const ReportLost& entry : report.lost) {
                json item = {{"color", jasperengine::pixelart::toHex(entry.color, false)},
                             {"became", jasperengine::pixelart::toHex(entry.became, false)},
                             {"pixels", entry.pixels},
                             {"at", entry.at}};
                if (entry.frame >= 0) item["frame"] = entry.frame;
                lost.push_back(std::move(item));
            }
            out["palette"] = {{"name", report.paletteName}, {"lost", std::move(lost)}};
        }
        if (report.animation) {
            const ReportAnimation& a = *report.animation;
            json clips = json::array();
            for (const ReportClip& clip : a.clips) {
                clips.push_back({{"name", clip.name}, {"frames", clip.frames}, {"include", clip.include}, {"fps", clip.fps},
                                 {"loop", loopName(clip.loop)}, {"width", clip.width}, {"height", clip.height}});
            }
            json boxes = json::array();
            for (const ReportBox& box : a.boxes) {
                json item = {{"frame", box.frame}, {"x", box.box.x}, {"y", box.box.y}, {"width", box.box.width}, {"height", box.box.height}, {"include", box.include}};
                if (box.cell) item["cell"] = *box.cell;
                if (box.confidence) item["confidence"] = *box.confidence;
                boxes.push_back(std::move(item));
            }
            out["animation"] = {{"frames", a.frames}, {"included", a.included}, {"anchor", anchorName(a.anchor)}, {"animations", std::move(clips)}, {"boxes", std::move(boxes)}};
        }
        return out;
    }

}
