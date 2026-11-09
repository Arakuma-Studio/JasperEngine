// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// S42's own rules (R10), one pure function each: PixelArtCheck::run calls them in the table's order.
// Every rule appends its findings sorted by key, with no id (run numbers them).

#include "pixelart/PixelArtCheck.h"

#include "Engine.h"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::rules {

    RuleResult pixelArtMode(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult integerScale(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult filtering(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult mipmapsCompression(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult palette(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult texelSize(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult offGrid(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult rotation(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult fontAntialiasing(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult particlesGrid(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult antiAliasing(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult renderScale(const PixelArtCheckInput& input, std::vector<Finding>& findings);
    RuleResult spriteVariants(const PixelArtCheckInput& input, std::vector<Finding>& findings);

    // the font analysis analyzeFonts made for the input, else setFontAnalyzer's now
    PixelArtFontInfo analyzeFont(const PixelArtCheckInput& input, const std::vector<std::string>& fonts, unsigned int size);

    // the per-screen layouts of a scene's main camera (integer_scale's measurement, the result's camera[])
    nlohmann::json cameraJson(const PixelArtCheckInput& input, const PixelArtScene& scene);

    // the preset's project part for this project (R2 rows 1-6 with the options a project already
    // satisfies kept, and every Device Tier's Render Scale cap below 1 raised to 1): the changes
    // pixel_art_mode's fix and the preset share
    std::vector<FixChange> projectPresetChanges(const PixelArtProjectInfo& project, int baseWidth, int baseHeight, Scaling scaling,
                                                int pixelsPerUnit, int windowScale);

    // what puts an enabled orthographic Pixel Perfect Camera on a scene's main camera (enable it,
    // add it, or add a Main Camera with it; reference = the fixed resolution if set, else the
    // canvas): integer_scale's fix, and particles_grid's when nothing snaps; empty for a
    // perspective camera. label: the option's label
    std::vector<FixChange> cameraFixChanges(const PixelArtCheckInput& input, const PixelArtScene& scene, std::string& label);
    // every Device Tier Render Scale cap below 1 set to 1 (render_scale's project fix, the preset's)
    std::vector<FixChange> renderScaleCapChanges(const PixelArtProjectInfo& project);

    // "nearest", "linear", ...: TextureFilter as set_properties names it
    const char* filterName(TextureFilter filter);
    // "letterbox", "integer" ...: Scaling as graphics.scaling_mode names it
    const char* scalingName(Scaling scaling);

}
