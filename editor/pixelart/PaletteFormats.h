// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Foreign palette files (docs/specs/S43-palettes.md R5, R6): GIMP .gpl, Lospec .hex, JASC and
// Microsoft RIFF .pal, Paint.NET .txt, Adobe Swatch Exchange .ase, images and swatch strips, and
// the editor's .colors swatch libraries, read into a Palette and written back out. Reading sniffs
// the content first and the extension second. Every writer reads back through its reader to the
// same colour list; output is byte-deterministic. Aseprite files are recognised by their magic
// and refused until S44's importer is in.

#include "pixelart/Palette.h"
#include "pixelart/PixelProblem.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jasperengine::editor::pixelart {

    enum class PaletteFormat : uint8_t { Gpl, Hex, JascPal, RiffPal, PaintNetTxt, AdobeAse, Aseprite, Image, SwatchLibrary, PngStrip };

    // the MCP names: gpl, hex, pal, pal_riff, txt, ase, aseprite, image, colors, png
    const char* paletteFormatName(PaletteFormat format);
    bool parsePaletteFormat(std::string_view name, PaletteFormat& out);
    // the extension a format is written with (".gpl", ".pal", ".png", ...)
    const char* paletteFormatExtension(PaletteFormat format);
    // every format but Aseprite and Image (an image is written as a PngStrip)
    bool canExportPalette(PaletteFormat format);

    // kind is one of format_unsupported, too_many_colors, parse_error (import) or invalid_value
    // (export); the line or byte offset of a parse_error is in its message
    using PaletteError = PixelProblem;

    // The result of an import: `palette` on success, else `error` (its kind is empty on success).
    // `format` is what the bytes were read as; it means nothing when error.kind is
    // format_unsupported. Warnings (converted CMYK, clamped HDR, duplicate colours, ...) come
    // with a palette and with errors alike.
    struct ImportResult {
        std::optional<jasperengine::pixelart::Palette> palette;
        PaletteFormat format = PaletteFormat::Gpl;
        std::vector<PixelProblem> warnings;
        PixelProblem error;

        bool ok() const { return palette.has_value(); }
    };

    // Reads a palette from the bytes of `path` (the path names the palette and gives the
    // extension; the file is not read). The palette gets the file's name (else the file stem),
    // source {kind: file, file: <file name>} and the format's column count when it has one.
    // More than 256 colours is too_many_colors, unless firstColorsOnly keeps the first 256 (with
    // a warning).
    ImportResult importPalette(const std::filesystem::path& path, std::span<const uint8_t> bytes, bool firstColorsOnly = false);

    // The bytes of a palette in a format. Alpha is written only by the formats that hold it
    // (hex, txt, colors, png); the others write the colours opaque and add a warning. PngStrip
    // draws each colour as a swatchSize x swatchSize square in one row (1 to 64). The .colors
    // library holds the first 76 colours (warning when there are more).
    std::variant<std::vector<uint8_t>, PaletteError> exportPalette(const jasperengine::pixelart::Palette& palette, PaletteFormat format,
                                                                   std::vector<std::string>& warnings, int swatchSize = 1);

}
