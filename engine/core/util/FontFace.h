// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FONTFACE_H
#define FONTFACE_H

#include "Export.h"
#include "stb_truetype.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

//HarfBuzz objects, kept opaque so its headers stay out of the engine API
struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;

namespace jasperengine {

    // Whether a face needs stb_truetype (the text atlases rasterize with it). Optional also opens
    // fonts stb_truetype cannot (CFF2 variable OpenType) for shaping and HarfBuzz outlines only
    // (Title Art's glyph outlines, S80 R21): their rasterizeGlyph draws nothing.
    enum class FontFaceRasterizer : uint8_t { Required, Optional };

    // One font file (or collection member), shared by every size and user: STBText chains
    // and the HTML UI font engine hold it by shared_ptr. Immutable after loading, except for
    // HarfBuzz's lazily loaded tables (HarfBuzz is built without thread safety, HB_NO_MT:
    // shape a face from one thread at a time, which is the main thread for both users).
    class JASPERENGINE_API FontFace {
    public:
        // TTF/OTF/TTC and WOFF/WOFF2 (decoded to owned SFNT bytes, at most 64 MiB).
        // Read through the engine's file system (Data::open: project-relative, asset://,
        // data://, resource packs). On failure returns nullptr and, when `error` is given,
        // writes why ("Font file not found: <path>", "Failed to initialize font: <path>");
        // nothing is logged here, callers log with their own prefix.
        static std::shared_ptr<FontFace> loadFile(const std::string& path, int faceIndex = 0, std::string* error = nullptr,
                                                  FontFaceRasterizer rasterizer = FontFaceRasterizer::Required);
        // copy = false keeps a pointer to `data`, which must outlive the face (built-in fonts)
        static std::shared_ptr<FontFace> loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                                    bool copy, const std::string& label, std::string* error = nullptr,
                                                    FontFaceRasterizer rasterizer = FontFaceRasterizer::Required);
        ~FontFace();

        FontFace(const FontFace&) = delete;
        FontFace& operator=(const FontFace&) = delete;

        const std::string& getLabel() const;        // path or label
        const std::string& getFamilyName() const;   // name table ID 16, else 1 (UTF-16BE -> UTF-8)
        int getWeight() const;                      // OS/2 usWeightClass, 400 when absent
        bool isItalic() const;                      // OS/2 fsSelection bit 0 (or 9, oblique), else head.macStyle bit 1
        int getFaceIndex() const;

        int getUnitsPerEm() const;
        int getAscender() const; int getDescender() const; int getLineGap() const;   // hhea, font units (descender negative)
        int getXHeight() const;            // OS/2 sxHeight (version >= 2), else bbox of 'x', else 0.5 * ascender
        int getUnderlinePosition() const;  // post, negative below baseline; 0 if no post table
        int getUnderlineThickness() const; // post; 0 if no post table
        bool hasPostTable() const;

        uint32_t getGlyphIndex(uint32_t codepoint) const;   // 0 = not covered
        // (ASCII from a bitmap read at load: font fallback asks it for every codepoint)
        bool hasCodepoint(uint32_t codepoint) const{
            if (codepoint < 128)
                return ((asciiCoverage[codepoint >> 6] >> (codepoint & 63)) & 1) != 0;
            return getGlyphIndex(codepoint) != 0;
        }
        float scaleForPixelHeight(float pixels) const;      // stbtt_ScaleForPixelHeight (STBText semantics)
        float scaleForEmSize(float pixels) const;           // stbtt_ScaleForMappingEmToPixels (CSS font-size)
        // Coverage bitmap (R8, rows of `width` bytes) of one glyph at a scale; offsets of its
        // top-left corner relative to the pen on the baseline, y down. False for a glyph
        // without ink (a space): pixels empty, size 0, offsets still set.
        bool rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                            int& width, int& height, int& xoff, int& yoff) const;
        float getGlyphAdvance(uint32_t glyphIndex, float scale) const;

        // false for a face opened with FontFaceRasterizer::Optional that stb_truetype cannot read:
        // metrics, coverage and advances come from HarfBuzz, rasterizeGlyph draws nothing
        bool canRasterize() const;
        const stbtt_fontinfo& getInfo() const;
        hb_font_t* getHbFont() const;   // scale (upem, upem): positions in font units

    private:
        FontFace() = default;

        bool init(int faceIndex, std::string* error, FontFaceRasterizer rasterizer);
        void readTables();

        std::vector<unsigned char> storage;     // owned bytes (files, copies)
        const unsigned char* bytes = nullptr;   // storage.data() or the caller's memory
        size_t length = 0;

        stbtt_fontinfo info = {};
        bool rasterizable = true;   // stb_truetype read the face
        hb_blob_t* blob = nullptr;
        hb_face_t* face = nullptr;
        hb_font_t* font = nullptr;

        std::string label;
        std::string familyName;
        int faceIndex = 0;
        int weight = 400;
        bool italic = false;
        int unitsPerEm = 0;
        int ascender = 0;
        int descender = 0;
        int lineGap = 0;
        int xHeight = 0;
        int underlinePosition = 0;
        int underlineThickness = 0;
        bool postTable = false;
        uint64_t asciiCoverage[2] = {};
    };

}

#endif //FONTFACE_H
