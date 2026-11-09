// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelLint.h"

#include "pixelart/Palette.h"
#include "pixelart/lint/CraftLints.h"
#include "pixelart/lint/EffectLints.h"

#include <algorithm>

namespace jasperengine::editor::pixelart::lint {

    namespace {

        // R26's catalogue, in report order
        constexpr const char* kLintIds[] = {
            "off_palette", "colors", "partial_alpha", "transparent_background", "size", "near_duplicates",
            "orphans", "jaggies", "doubles", "holes", "flicker", "contrast", "banding", "pillow_shading",
            "empty_frame", "one_frame", "frame_scale",
        };

        // R26's defaults (ritmo's where it has them)
        LintThreshold catalogueThreshold(const LintOptions& options, const char* id, double fallback, const std::string& source = "default") {
            LintThreshold t;
            const auto it = options.thresholds.find(id);
            if (it != options.thresholds.end()) {
                t.value = it->second;
                t.source = std::string("options.") + id;
            } else {
                t.value = fallback;
                t.source = source;
            }
            return t;
        }

        bool catalogueOverridden(const LintOptions& options, const char* id) {
            return options.thresholds.find(id) != options.thresholds.end();
        }

    }

    std::span<const char* const> lintIds() {
        return kLintIds;
    }

    bool isLintId(std::string_view id) {
        return std::any_of(std::begin(kLintIds), std::end(kLintIds), [&](const char* known) { return id == known; });
    }

    std::string lintIdList() {
        std::string list;
        for (const char* id : kLintIds) list += (list.empty() ? "" : ", ") + std::string(id);
        return list;
    }

    LintReport lint(const LintInput& input, const LintOptions& options) {
        LintReport report;
        auto wanted = [&](const char* id) {
            return options.only.empty() || std::find(options.only.begin(), options.only.end(), id) != options.only.end();
        };
        if (input.frames.empty()) {
            for (const char* id : kLintIds) {
                if (!wanted(id)) continue;
                LintResult r;
                r.id = id;
                r.status = LintStatus::Skip;
                r.thresholdSource = "default";
                r.message = "no frames given";
                report.add(std::move(r));
            }
            return report;
        }
        const FrameView frames = frameView(input.frames);
        std::vector<LintAnimation> animations = input.animations;

        if (wanted("off_palette")) {
            OffPaletteOptions o;
            // R26: any opaque pixel off the palette FAILs
            o.warnCount = 0.0;
            o.failShare = 0.0;
            if (catalogueOverridden(options, "off_palette")) {
                o.failShare = options.thresholds.at("off_palette");
                o.thresholdSource = "options.off_palette";
            }
            std::span<const jasperengine::pixelart::Rgba8> colors;
            if (input.palette) {
                colors = input.palette->colors;
                o.paletteName = input.palette->name.empty() ? std::string("the palette") : input.palette->name;
            }
            LintResult r = lintOffPalette(frames, colors, o);
            if (o.thresholdSource != "default") r.threshold = o.failShare;
            report.add(std::move(r));
        }
        if (wanted("colors")) {
            std::optional<LintThreshold> max;
            if (input.rules.maxColors) {
                max = LintThreshold{(double)*input.rules.maxColors, "rules.max_colors"};
            }
            if (catalogueOverridden(options, "colors")) max = catalogueThreshold(options, "colors", 0.0);
            report.add(lintColors(frames, max ? &*max : nullptr));
        }
        if (wanted("partial_alpha")) {
            LintResult r = lintPartialAlpha(frames);
            if (r.status == LintStatus::Fail) r.fix = "fix_pixel_art: alpha_cutoff (default 128) makes every pixel fully opaque or fully transparent";
            report.add(std::move(r));
        }
        if (wanted("transparent_background")) report.add(lintTransparentBackground(frames, input.rules.transparentBackground));
        if (wanted("size")) report.add(lintSize(frames, input.rules.sizes));
        if (wanted("near_duplicates")) report.add(lintNearDuplicates(frames, catalogueThreshold(options, "near_duplicates", 0.03)));
        if (wanted("orphans")) report.add(lintOrphans(frames, input.sheet, catalogueThreshold(options, "orphans", 1)));
        if (wanted("jaggies")) report.add(lintJaggies(frames, catalogueThreshold(options, "jaggies", CRAFT_SPOTS)));
        if (wanted("doubles")) report.add(lintDoubles(frames, catalogueThreshold(options, "doubles", CRAFT_SPOTS)));
        if (wanted("holes")) report.add(lintHoles(frames, catalogueThreshold(options, "holes", CRAFT_SPOTS)));
        if (wanted("flicker")) {
            const LintThreshold t = catalogueThreshold(options, "flicker", 0.06);
            LintResult r = lintFlicker(frames, input.loop, t.value);
            if (r.status != LintStatus::Skip) r.thresholdSource = t.source;
            if (r.status == LintStatus::Warn && !r.at.empty()) {
                r.fix = "edit_sprite: hold the pixel at " + std::to_string(r.at.front().x) + "," + std::to_string(r.at.front().y) + " of frame " +
                        std::to_string(r.at.front().frame) + " for two frames, or change it back later (S47)";
            }
            report.add(std::move(r));
        }
        if (wanted("contrast")) report.add(lintContrast(frames, input.backdrops, catalogueThreshold(options, "contrast", 0.5, input.backdropsSource)));
        if (wanted("banding")) report.add(lintBanding(frames, catalogueThreshold(options, "banding", 12)));
        if (wanted("pillow_shading")) report.add(lintPillowShading(frames, catalogueThreshold(options, "pillow_shading", 0.6)));
        if (wanted("empty_frame")) report.add(lintEmptyFrame(frames, animations));
        if (wanted("one_frame")) report.add(lintOneFrame(animations));
        if (wanted("frame_scale")) report.add(lintFrameScale(input.frameCells, input.sharedCell, catalogueThreshold(options, "frame_scale", 0.05)));
        return report;
    }

}
