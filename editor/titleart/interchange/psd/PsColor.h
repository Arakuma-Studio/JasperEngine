// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R44, §6.9)
// - Purpose: Photoshop colours as sRGB "#rrggbb", as Pachislop's psStyle.ts converts them: RGB
//   (0..255 or float), HSB, CMYK (naive), grey and Lab (D50, Bradford-adapted to sRGB). Colours
//   are read as sRGB values; no profile conversion (R44).
// - Thread-safety: pure functions.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>

namespace jasperengine::editor::psd {

    // a colour descriptor (RGBC, HSBC, CMYC, Grsc, LbCl, or the float RGB of newer files)
    std::string psColorHex(const titleart::Json& color, const std::string& fallback = "#000000");
    std::string rgbHex(double r, double g, double b);
    // a over b at t (sRGB values, as psStyle.ts mixes)
    std::string mixHex(const std::string& a, const std::string& b, double t);
    void hexRgb(const std::string& hex, double& r, double& g, double& b);

}
