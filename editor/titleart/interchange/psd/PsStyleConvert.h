// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R32, R44, §6.9)
// - Purpose: Photoshop layer styles (a layer's `lfx2` / `lmfx`, an ASL style's `Lefx`) as Title
//   Art styles and effects: a port of Pachislop's src/io/psStyle.ts (readPsStyles, convertPsStyle)
//   by SafEight, used with the author's permission, with §6.9's improvements: gradient opacity
//   stops kept as stop `alpha` and midpoints as `mid`, stroke opacity as the rim material's
//   opacity, bitmap patterns extracted as `custom` patterns, every instance of the multi-effect
//   (`lmfx`) lists, Shape Burst read natively, Bevel direction Down as `bevel.direction: down`,
//   Emboss and Pillow Emboss as an S83 `material` effect, Inner Glow, Outer Glow, Drop Shadow and
//   Inner Shadow as S80's effects.
// - Sizes: style members are % of the reference size (the PS text layer's font size, else the
//   layer height, else 100 px); effect params stay px for that size (`ref_size`), so applying to a
//   layer scales them by its size / ref_size.
// - `pachislopLiteral` (tests only, AC16's red control) converts as Pachislop does: transparency
//   stops ignored, midpoints as extra mixed stops, strokes opaque, bitmap patterns skipped.
// - Thread-safety: pure functions.

#pragma once

#include "AslReader.h"
#include "PsdDescriptor.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    struct PsStyleSource {
        std::string name;
        std::string origin;             // "text layer", "layer", "style library"
        Json effects;                   // the effects descriptor
        std::string baseColor = "#ffffff";  // the face colour under the overlays (the text colour)
        double fontPx = 0;              // the PS text layer's font size in px (0: unknown)
        double suggestedPx = 100;       // the size offered when the font size is unknown
        std::string sizeNote;           // how the size was found ("font 48 pt", "layer height")
        double globalAngle = 120, globalAltitude = 30;
        double ppi = 72;
        double fillOpacity = 1;
        bool knockout = false;
    };

    struct PsStyleFile {
        std::string kind;               // "psd" or "asl"
        std::vector<PsStyleSource> styles;
        std::vector<PsPattern> patterns;
        std::vector<std::string> skipped;
    };

    // the styles of a .psd (every layer with an enabled effect, top to bottom as the Layers
    // panel lists them) or an .asl; false with `error`
    bool readPsStyles(const uint8_t* data, size_t size, PsStyleFile& out, std::string& error);
    // the enabled effects of a descriptor exist (masterFXSwitch on, one effect enabled)
    bool psEffectsEnabled(const Json& effects);

    struct PsConvertOptions {
        bool pachislopLiteral = false;
        // writes a bitmap pattern as a project image; returns its assets-relative path ("" refuses)
        std::function<std::string(const PsPattern&)> savePattern;
    };

    struct ConvertedPsStyle {
        Json style;                     // style2d: face, rims, knockout
        Json effects = Json::array();   // S80 / S83 effect entries {type, enabled, params}
        double refSize = 100;
        std::vector<std::string> summary;   // what was mapped
        std::vector<std::string> notes;     // approximations and dropped settings
    };

    // false with `error` when sizePx <= 0
    bool convertPsStyle(const PsStyleSource& source, double sizePx, const std::vector<PsPattern>& patterns,
                        const PsConvertOptions& options, ConvertedPsStyle& out, std::string& error);

    // the reference size of a source: its font size, else the suggested one
    double psReferenceSize(const PsStyleSource& source);

    // Photoshop blend keys and descriptor blend enums as S80 blend names ("" when S80 lacks it)
    std::string titleBlendOfPsKey(const std::string& key);
    std::string titleBlendOfPsEnum(const std::string& value);
    // the PSD blend key of an S80 blend ("" when Photoshop lacks it)
    std::string psKeyOfTitleBlend(const std::string& blend);

}
