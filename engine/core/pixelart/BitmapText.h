// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPTEXT_H
#define PIXELART_BITMAPTEXT_H

// A Text's bitmap font chain laid out in whole font pixels (S51 R13) and written as quads `unit`
// units per font pixel (D3): the same outputs as STBText::createText (buffer, indices,
// charPositions, charExtents, glyphInfo, width, height), so caret, selection, Text Effects and
// visibleCharacters work unchanged. Unstyled single-page fonts draw from the font's own atlas;
// a style (outline, shadow) or a font of several pages draws from the styled glyph cache as two
// quad layers, every back quad before every face quad (D5).

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "pixelart/BitmapTextCache.h"
#include "util/CharExtent.h"
#include "util/TextGlyphInfo.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Buffer;
    class TextureData;
}

namespace jasperengine::pixelart {

    class BitmapFont;

    struct BitmapTextOptions {
        float unit = 1.0f;              // units per font pixel
        bool fixedWidth = false;
        bool fixedHeight = false;
        bool multiline = true;
        bool invert = false;            // y up (STBText's invert)
        BitmapTextStyle style;
        bool vertexColor = false;       // the buffer has a COLOR attribute (forced by the cache path)
        Vector3 faceColor = Vector3(1.0f, 1.0f, 1.0f);   // linear, the face quads' vertex colour
    };

    // one laid-out line, font pixels
    struct BitmapLine {
        size_t first = 0, count = 0;    // codepoints (the break's swallowed space excluded)
        int width = 0;                  // advance width, no trailing spacing
    };

    class JASPERENGINE_API BitmapText {
    public:
        BitmapText(std::shared_ptr<BitmapFont> font, std::string chainKey, uint32_t generation);

        const std::shared_ptr<BitmapFont>& getFont() const { return font; }
        const std::string& getChainKey() const { return chainKey; }
        uint32_t getGeneration() const { return generation; }

        // the layout of R13: newlines, word wrap at widthPx when wrapping (fixedWidth and multiline)
        static std::vector<BitmapLine> layoutLines(const BitmapFont& font, const std::vector<uint32_t>& codepoints, bool wrap, int widthPx);
        // the size a string takes, font pixels: the widest line (advances, or ink past them) and
        // the lines times the line height (Lua BitmapFont:measure)
        static void measure(const BitmapFont& font, const std::string& text, int& width, int& height);

        // width and height in and out are in units (whole units, as the layout rect)
        void createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                        std::vector<CharExtent>& charExtents, unsigned int& width, unsigned int& height, const BitmapTextOptions& options,
                        std::vector<TextGlyphInfo>* glyphInfo);

        // what the last createText drew from
        bool usesCache() const { return cachePage >= 0; }
        int getCachePage() const { return cachePage; }
        uint32_t getCacheGeneration() const { return cacheGeneration; }
        uint32_t getCacheVersion() const { return cacheVersion; }
        int getBackQuads() const { return backQuads; }
        TextureData* getTexture() const;
        std::string getTextureId() const;
        bool isColoredTexture() const;  // RGBA (coloured font or the cache), else R8 coverage
        // the cache page was recycled since: the text builds again
        bool isCacheStale() const;
        const BitmapTextStyle& getLastStyle() const { return lastStyle; }
        // the font ascent (its baseline) in units
        float getAscent(float unit) const;

        // the pool revision its generation was last compared at (UISystem)
        uint32_t poolRevision = 0;

    private:
        std::shared_ptr<BitmapFont> font;
        std::string chainKey;
        uint32_t generation = 0;
        int page = 0;                   // the font's page (direct)
        int cachePage = -1;
        uint32_t cacheGeneration = 0;
        uint32_t cacheVersion = 0;
        int backQuads = 0;
        BitmapTextStyle lastStyle;
        std::vector<uint32_t> acquiredText;     // the codepoints of the last cache acquire
    };

}

#endif //PIXELART_BITMAPTEXT_H
