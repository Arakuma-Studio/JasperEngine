// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R39, R44, D7, §6.10)
// - Purpose: our own sRGB ICC profile, the tag the PSD writer embeds (resource 0x040F): ICC v2.1,
//   display class, RGB data, XYZ PCS, D50; tags desc "sRGB (JasperEngine)", cprt, wtpt (D50),
//   rXYZ / gXYZ / bXYZ (the sRGB primaries and D65 white of IEC 61966-2-1, Bradford-adapted to
//   the ICC D50 illuminant, as s15Fixed16) and one shared 1,024-entry `curv` of the IEC 61966-2-1
//   transfer that rTRC, gTRC and bTRC all point to. No third-party profile: no licence question,
//   and never the Japan Color profile.
// - Built from constants with integer rounding, so the bytes are the same on every platform; no
//   date or profile id is written.
// - Thread-safety: the bytes are made once (a function-local static).

#pragma once

#include <cstdint>
#include <vector>

namespace jasperengine::editor {

    const std::vector<uint8_t>& srgbIccProfile();

    // the matrix (columns r, g, b; rows X, Y, Z) the profile stores, before s15Fixed16 rounding
    void srgbIccMatrix(double out[3][3]);
    // the IEC 61966-2-1 transfer: encoded 0..1 to linear 0..1
    double srgbIccTransfer(double encoded);

}
