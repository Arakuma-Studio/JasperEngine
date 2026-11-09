// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteDrawLint.h"

#include "pixelart/Palette.h"
#include "pixelart/lint/PixelLint.h"

#include <algorithm>
#include <set>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        namespace ase = aseprite;

        // S46's spots count from 1 in the frames it was given: back to the sprite's numbers
        nlohmann::json resultJson(lint::LintResult result, const std::vector<int>& frames) {
            for (lint::LintSpot& spot : result.at) {
                const int index = spot.frame - 1;
                if (index >= 0 && index < static_cast<int>(frames.size())) spot.frame = frames[static_cast<size_t>(index)] + 1;
            }
            return lint::toJson(result);
        }

    }

    bool validateLintArg(const nlohmann::json& value, PixelProblem& problem) {
        if (value.is_boolean()) return true;
        if (value.is_array()) {
            for (const nlohmann::json& id : value) {
                if (!id.is_string() || !lint::isLintId(id.get<std::string>())) {
                    problem = {"bad_op_field", "lint", "unknown lint " + id.dump(), "lint is true or a list of: " + lint::lintIdList()};
                    return false;
                }
            }
            return true;
        }
        problem = {"bad_op_field", "lint", "lint must be true or a list of lint ids", "lint is true or a list of: " + lint::lintIdList()};
        return false;
    }

    nlohmann::json lintAfterDraw(const SpriteDoc& sprite, const std::vector<DirtyRect>& changed, const nlohmann::json& value,
                                 const std::vector<jasperengine::pixelart::Rgba8>& backdrops) {
        const ase::Document& doc = sprite.doc;
        std::vector<std::string> only;
        const bool named = value.is_array();
        if (named) {
            for (const nlohmann::json& id : value) only.push_back(id.get<std::string>());
        }
        std::set<int> touched;
        for (const DirtyRect& d : changed) touched.insert(d.frame - 1);
        if (touched.empty()) touched.insert(0);

        jasperengine::pixelart::Palette palette;
        for (const ase::PaletteEntry& e : doc.palette) palette.colors.push_back({e.color[0], e.color[1], e.color[2], e.color[3]});
        const bool hasPalette = !palette.colors.empty() && sprite.indexed();

        auto composite = [&](int frame) { return ase::renderFrame(doc, frame); };

        lint::LintReport report;
        nlohmann::json results = nlohmann::json::array();
        auto take = [&](const lint::LintResult& result, const std::vector<int>& frames) {
            report.add(result);
            const bool show = result.status == lint::LintStatus::Warn || result.status == lint::LintStatus::Fail || (result.status == lint::LintStatus::Skip && named);
            if (show) results.push_back(resultJson(result, frames));
        };

        const bool wantFlicker = !named || std::find(only.begin(), only.end(), "flicker") != only.end();
        std::vector<int> frames(touched.begin(), touched.end());
        std::vector<PixelImage> images;
        for (int f : frames) images.push_back(composite(f));
        lint::LintInput input;
        input.frames = images;
        input.palette = hasPalette ? &palette : nullptr;
        input.backdrops = backdrops;
        input.backdropsSource = "pixel_art.backdrops";
        lint::LintOptions options;
        options.only = only;
        // flicker reads an animation: below, on the touched tag's frames
        if (!named) {
            for (const char* id : lint::lintIds()) {
                if (std::string(id) != "flicker") options.only.push_back(id);
            }
        } else {
            options.only.erase(std::remove(options.only.begin(), options.only.end(), "flicker"), options.only.end());
        }
        if (!options.only.empty()) {
            const lint::LintReport part = lint::lint(input, options);
            for (const lint::LintResult& result : part.results) take(result, frames);
        }
        if (wantFlicker) {
            for (const ase::Tag& tag : doc.tags) {
                const bool hit = std::any_of(frames.begin(), frames.end(), [&](int f) { return f >= tag.from && f <= tag.to; });
                if (!hit) continue;
                std::vector<int> tagFrames;
                std::vector<PixelImage> tagImages;
                for (int f = tag.from; f <= tag.to && f < doc.frameCount(); f++) {
                    tagFrames.push_back(f);
                    tagImages.push_back(composite(f));
                }
                lint::LintInput tagInput;
                tagInput.frames = tagImages;
                tagInput.palette = input.palette;
                tagInput.loop = tag.direction == ase::TagDirection::Forward || tag.direction == ase::TagDirection::Reverse;
                lint::LintOptions tagOptions;
                tagOptions.only = {"flicker"};
                const lint::LintReport part = lint::lint(tagInput, tagOptions);
                for (const lint::LintResult& result : part.results) take(result, tagFrames);
            }
        }
        return {{"counts", {{"pass", report.pass}, {"warn", report.warn}, {"fail", report.fail}, {"skip", report.skip}}}, {"results", std::move(results)}};
    }

}
