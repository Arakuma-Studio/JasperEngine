// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef IESPROFILE_H
#define IESPROFILE_H

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine {

    // A light's IES Profile (spec S99 R36): an IESNA LM-63-1995 or -2002 photometric file
    // (TILT=NONE, Type C photometry) resampled to a fixed grid by its symmetry and normalised
    // to a peak of 1, the light's Intensity staying its peak. Rows of the grid go round the axis
    // (horizontal angle 0..360 degrees, IES_GRID_ROWS rows, the last equal to the first), columns
    // away from it (vertical angle 0..180 degrees: 0 along the light's forward axis, the
    // luminaire's nadir). The renderer packs every profile of a scene into one texture, a band of
    // IES_GRID_ROWS rows each (includes/lights.glsl).
    struct JASPERENGINE_API IesProfile {
        static constexpr int GRID_COLUMNS = 128;
        static constexpr int GRID_ROWS = 33;

        // LM-63-1995, LM-63-2002 (or LM-63-1991 / 1986, read the same way)
        std::string format;
        std::string manufacturer;     // [MANUFAC]
        std::string luminaire;        // [LUMINAIRE], else [LUMCAT]
        std::string lamp;             // [LAMP], else [LAMPCAT]
        int lamps = 1;
        float lumensPerLamp = 0.0f;   // -1: absolute photometry
        float lumens = 0.0f;          // the lamps' total (lumensPerLamp x lamps), else integrated from the candela
        float peakCandela = 0.0f;     // the file's largest candela value (with its multiplier)
        // what the file's horizontal angles say of its symmetry
        enum class Symmetry { FULL, ROTATIONAL, QUADRANT, BILATERAL, BILATERAL_90 } symmetry = Symmetry::FULL;
        std::vector<float> verticalAngles;   // degrees, as the file lists them
        std::vector<float> horizontalAngles;
        // GRID_ROWS x GRID_COLUMNS, row major, 0..1
        std::vector<float> grid;

        // the profile at a direction of the light's frame: vertical angle from its forward axis,
        // horizontal angle round it (degrees), bilinear in the grid as the shader samples it
        float sample(float verticalDegrees, float horizontalDegrees) const;
        // the file's own value there (before resampling), normalised to its peak
        float sampleSource(float verticalDegrees, float horizontalDegrees) const;

        static const char* symmetryName(Symmetry symmetry);

        // Reads LM-63 text. False with a message naming the problem (TILT=INCLUDE or a tilt file,
        // another photometric type, a short or malformed table)
        static bool parse(const std::string& text, IesProfile& out, std::string& error);

    private:
        std::vector<float> candela;  // horizontal-major, as the file lists them, normalised
    };

}

#endif //IESPROFILE_H
