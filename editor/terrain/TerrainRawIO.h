// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R61, R68: heightmap files in and out, shared by edit_terrain's import_heightmap,
// export_terrain_heightmap and the Terrain window's Import Raw... and Export Raw... dialogs. A raw file
// says nothing about itself, so its size, depth and byte order are guessed from its length and its
// smoothness, and every guess is reported; an ambiguity refuses, naming the candidates.

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    enum class HeightFileFormat{
        Auto,
        R8,
        R16,
        R32F,
        Png,
        Exr
    };

    enum class HeightByteOrder{
        Auto,
        Little,     // Windows
        Big         // Mac
    };

    enum class HeightRange{
        Auto,       // the data's own minimum and maximum
        Full,       // the format's: 0-255, 0-65535, 0-1 for floats
        Source      // sourceMin and sourceMax
    };

    struct HeightImportOptions{
        HeightFileFormat format = HeightFileFormat::Auto;
        int width = 0;              // 0: detected
        int height = 0;
        HeightByteOrder byteOrder = HeightByteOrder::Auto;
        bool flipVertically = false;
        HeightRange range = HeightRange::Auto;
        float sourceMin = 0.0f;
        float sourceMax = 1.0f;
    };

    // What was read, with what was guessed
    struct HeightImport{
        bool ok = false;
        std::string error;
        HeightFileFormat format = HeightFileFormat::R16;
        HeightByteOrder byteOrder = HeightByteOrder::Little;
        int width = 0;
        int height = 0;
        // which of format, size and byte order were detected rather than given
        bool formatGuessed = false;
        bool sizeGuessed = false;
        bool byteOrderGuessed = false;
        // the stored values' range and the one mapped to 0..1
        double dataMin = 0.0;
        double dataMax = 0.0;
        double rangeLow = 0.0;
        double rangeHigh = 1.0;
        // 0..1, row by row, rows top first unless flipped; values past the range clamp and count
        std::vector<float> normalized;
        int64_t clippedLow = 0;
        int64_t clippedHigh = 0;
    };

    struct HeightExportOptions{
        HeightFileFormat format = HeightFileFormat::R16;
        HeightByteOrder byteOrder = HeightByteOrder::Little;
        bool flipVertically = false;
        // Full: 0..1 to the format's whole range; Auto: the data's own minimum and maximum to it
        HeightRange range = HeightRange::Full;
    };

    class TerrainRawIO{
    public:
        // The format a file name suggests; Auto when it suggests none (.raw)
        static HeightFileFormat formatOfName(const std::string& name);
        static const char* formatName(HeightFileFormat format);
        static bool formatFromName(const std::string& name, HeightFileFormat& out);

        // Reads a heightmap from its bytes. `name` gives the extension.
        static HeightImport decode(const std::string& name, const std::vector<uint8_t>& bytes, const HeightImportOptions& options);

        // Writes normalized heights (width x height, row by row) to bytes. EXR is written by the
        // caller through FloatImage, as it needs a path.
        static bool encode(const std::vector<float>& normalized, int width, int height, const HeightExportOptions& options,
                           std::vector<uint8_t>& bytes, std::string& error, double* rangeLow = nullptr, double* rangeHigh = nullptr);
    };

}
