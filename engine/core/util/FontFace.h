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

//HarfBuzz object, kept opaque so its headers stay out of the engine API
struct hb_font_t;

namespace jasperengine {

    // One font file (or collection member), shared by every size and user
    class JASPERENGINE_API FontFace {
    public:
        static std::shared_ptr<FontFace> loadFile(const std::string& path, int faceIndex = 0);
        static std::shared_ptr<FontFace> loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                                    bool copy, const std::string& label);
        ~FontFace();

        const std::string& getLabel() const;        // path or label
        const std::string& getFamilyName() const;   // name table ID 16, else 1 (UTF-16BE -> UTF-8)
        int getWeight() const;                      // OS/2 usWeightClass, 400 when absent
        bool isItalic() const;                      // OS/2 fsSelection bit 0, else head.macStyle bit 1

        int getUnitsPerEm() const;
        int getAscender() const; int getDescender() const; int getLineGap() const;   // hhea, font units (descender negative)
        int getXHeight() const;            // OS/2 sxHeight (version >= 2), else bbox of 'x', else 0.5 * ascender
        int getUnderlinePosition() const;  // post, negative below baseline; 0 if no post table
        int getUnderlineThickness() const; // post; 0 if no post table
        bool hasPostTable() const;

        uint32_t getGlyphIndex(uint32_t codepoint) const;   // 0 = not covered
        bool hasCodepoint(uint32_t codepoint) const;
        float scaleForPixelHeight(float pixels) const;      // stbtt_ScaleForPixelHeight (STBText semantics)
        float scaleForEmSize(float pixels) const;           // stbtt_ScaleForMappingEmToPixels (CSS font-size)
        // coverage bitmap of one glyph at a scale; offsets relative to pen/baseline, y down
        bool rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                            int& width, int& height, int& xoff, int& yoff) const;
        float getGlyphAdvance(uint32_t glyphIndex, float scale) const;

        const stbtt_fontinfo& getInfo() const;
        hb_font_t* getHbFont() const;   // scale (upem, upem): positions in font units

    private:
        stbtt_fontinfo info = {};
        std::string label;
        std::string familyName;
    };

}

#endif //FONTFACE_H
