// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R59)
// - Purpose: web fonts to the sfnt files Title Art loads. `.woff` (zlib tables through miniz) and
//   `.woff2` (Brotli, then the glyf/loca and hmtx transforms reversed, through the vendored
//   google/woff2 decoder) become TrueType (`.ttf`), CFF OpenType (`.otf`) or, for a WOFF2
//   collection, `.ttc` bytes. Also the faces of a `.ttc` (each face's family and style from its
//   name table), which the picker lists one by one.
// - Refusals, each with its reason: more than 64 MB decoded (checked against the header before
//   anything is inflated, then while decoding), a file shorter than its header says (truncated),
//   a table whose checksum is not the one the WOFF directory records, a zlib stream or a WOFF2
//   stream the decoder rejects.
// - Output: the WOFF tables in directory order, 4-byte aligned, with a fresh head
//   checkSumAdjustment (every other table byte for byte as stored).
// - Thread-safety: pure functions (call them from the Background lane: a large WOFF2 takes
//   tens of milliseconds).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::titlefonts {

    constexpr size_t MAX_DECODED_FONT_BYTES = 64ull * 1024 * 1024;

    enum class FontFileFormat : uint8_t { Unknown, TrueType, OpenTypeCff, Collection, Woff, Woff2 };

    // by the magic bytes (the extension is not trusted)
    FontFileFormat sniffFontFormat(const uint8_t* data, size_t size);
    // ".woff" / ".woff2" (case-insensitive)
    bool isWebFontFileName(const std::string& fileName);

    struct DecodedFont {
        bool ok = false;
        std::vector<uint8_t> bytes;     // the sfnt (or collection)
        std::string extension;          // ".ttf", ".otf" or ".ttc"
        std::string error;              // why it was refused
    };

    // WOFF or WOFF2 (by magic) to sfnt; a plain sfnt is refused ("not a web font")
    DecodedFont decodeWebFont(const uint8_t* data, size_t size, size_t maxDecoded = MAX_DECODED_FONT_BYTES);
    DecodedFont decodeWoff(const uint8_t* data, size_t size, size_t maxDecoded = MAX_DECODED_FONT_BYTES);
    DecodedFont decodeWoff2(const uint8_t* data, size_t size, size_t maxDecoded = MAX_DECODED_FONT_BYTES);

    // ".otf" for CFF / CFF2 outlines, ".ttc" for a collection, else ".ttf"
    std::string sfntExtension(const uint8_t* data, size_t size);

    // One face of a font file (a .ttc member, or the single face of a .ttf / .otf)
    struct FontFaceListing {
        int index = 0;
        std::string family;
        std::string style;
        std::string postScriptName;
    };
    // every face of the file (hb_face_count's answer, read from the collection header), empty
    // when it is not a font
    std::vector<FontFaceListing> listFontFaces(const uint8_t* data, size_t size);

}
