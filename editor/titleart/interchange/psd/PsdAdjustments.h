// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R33, R39)
// - Purpose: Photoshop adjustment layer records and S80 Adjustments, both ways: the 13 types the
//   importer maps (levl, curv, hue2, brit / CgEd, expA, vibA, blnc, blwh, phfl, grdm, nvrt, post,
//   thrs) and the 7 the PSD writer writes natively (levels, curves, hue_saturation,
//   brightness_contrast, invert, posterize, threshold). Formats from the Adobe Photoshop File
//   Formats Specification ("Adjustment layers"); the native-or-baked rules follow Pachislop
//   src/io/psd.ts (psdAdjustment, psCurve) by SafEight, used with the author's permission.
// - Thread-safety: pure functions.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    // the S80 adjustment object of a Photoshop record ({type: "levels", channels: {...}}); false with
    // `error` for a key Title Art has no adjustment for (selc, mixr, clrL) or malformed data
    bool psdAdjustmentToTitle(const std::string& key, const std::vector<uint8_t>& data, titleart::Json& out, std::string& error);

    // the Photoshop key and data of an S80 adjustment; false for the types written as "(baked)" and
    // for curves Photoshop cannot draw (corners, more than 16 points)
    bool titleAdjustmentToPsd(const titleart::Json& adjustment, std::string& key, std::vector<uint8_t>& data);

}
