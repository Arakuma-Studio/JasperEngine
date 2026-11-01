// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef STBText_h
#define STBText_h

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "buffer/InterleavedBuffer.h"
#include "io/Data.h"
#include "util/CharExtent.h"
#include "texture/Texture.h"
#include "util/TextShaper.h"
#include "util/TextGlyphInfo.h"

namespace jasperengine {

    class FontFace;
    class GlyphAtlas;

    // How an atlas rasterizes: rasterScale physical pixels per text pixel (crisp text on a
    // scaled canvas, Canvas Scaler); sdfPadding > 0 makes a signed-distance atlas with that
    // many pixels around each glyph (Text outline and shadow). The defaults are the plain atlas.
    struct JASPERENGINE_API STBTextOptions {
        float rasterScale = 1.0f;
        int sdfPadding = 0;
    };


    //glyph atlas of a font in a single size, plus the fonts it falls back to. The whole
    //chain shares one atlas, so mixed scripts stay a single texture. Faces come from
    //FontRegistry (parsed once, shared with the HTML UI), shaping from TextShaper and
    //packing from GlyphAtlas; layout, wrapping, atlas growth and the mesh stay here
    class JASPERENGINE_API STBText {

    private:

        struct FontGlyph {
            //quad corners relative to the pen position, y-down from the baseline
            float xoff;
            float yoff;
            float xoff2;
            float yoff2;

            float s0;
            float t0;
            float s1;
            float t1;

            float xadvance;
        };

        //max texture size guaranteed by GLES3 and WebGL2
        const unsigned int atlasLimit = 4096;
        const int atlasPadding = 1;

        std::vector<std::shared_ptr<FontFace>> faces;
        //the same chain for TextShaper, and each face's own scale: faces of different
        //unitsPerEm still render the same size
        std::vector<const FontFace*> facePointers;
        std::vector<float> faceScales;
        bool fontLoaded;

        //key is the face index and the glyph index of that face packed together
        std::unordered_map<uint64_t, FontGlyph> glyphMap;
        std::unordered_map<uint32_t, uint64_t> codepointMap;

        std::unique_ptr<GlyphAtlas> atlas;
        unsigned int atlasWidth;
        unsigned int atlasHeight;

        //atlases of previous sizes, TextureData points to their pixels without owning
        std::vector<std::unique_ptr<GlyphAtlas>> retiredAtlases;

        //bumped on every atlas change, texts built with an older one are outdated
        unsigned long atlasVersion;

        float ascent;
        float descent;
        float lineGap;
        int lineHeight;

        TextureData* textureData;

        STBTextOptions options;

        static uint64_t glyphKey(unsigned int face, uint32_t glyphIndex);

        bool addFace(const std::string& fontpath, unsigned int fontSize);
        void addFace(const std::shared_ptr<FontFace>& face, unsigned int fontSize);
        //the fonts compiled into the engine, Latin then Arabic
        void addBuiltInFaces(unsigned int fontSize);

        void shapeRange(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, unsigned int line, std::vector<ShapedGlyph>& shaped);
        void measureAdvances(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, std::vector<float>& advances);
        //shapes a candidate line alone and returns the width it really takes
        float shapeLineInto(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, unsigned int line, std::vector<ShapedGlyph>& lineGlyphs);
        //splits on newlines, wraps to width, and shapes each resulting line
        unsigned int shapeLines(const std::vector<uint32_t>& codepoints, unsigned int width, bool fixedWidth, bool multiline, std::vector<ShapedGlyph>& shaped);

        void resetAtlas(unsigned int width, unsigned int height);
        bool growAtlas();
        void rasterizeGlyph(unsigned int face, uint32_t glyphIndex, FontGlyph& glyph, int x, int y);
        const FontGlyph* getGlyph(unsigned int face, uint32_t glyphIndex);
        const FontGlyph* getSdfGlyph(unsigned int face, uint32_t glyphIndex, uint64_t key);
        const FontGlyph* getGlyphForCodepoint(uint32_t codepoint);
        void refreshTextureData();

    public:
        STBText();
        virtual ~STBText();

        //unique_ptr members are not copyable; MSVC instantiates the copy path unless these are deleted
        STBText(const STBText&) = delete;
        STBText& operator=(const STBText&) = delete;

        float getAscent();
        float getDescent();
        float getLineGap();
        int getLineHeight();
        float getCharWidth(uint32_t codepoint);

        unsigned long getAtlasVersion() const;

        // before load(): the raster scale and SDF padding of this atlas
        void setOptions(const STBTextOptions& options);
        const STBTextOptions& getOptions() const;
        // pixels of distance one unit of the SDF atlas's red channel spans (0..1 = 0..255):
        // distance = (value - 128/255) * getSdfDistanceScale(), positive inside the glyph
        float getSdfDistanceScale() const;

        TextureData* load(const std::string& fontpath, const std::vector<std::string>& fallbackPaths, unsigned int fontSize);
        // Every length in and out is in text pixels (UI units), whatever the raster scale.
        // glyphInfo (optional) gets one entry per quad written to the buffer.
        void createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                        std::vector<CharExtent>& charExtents,
                        unsigned int& width, unsigned int& height, bool fixedWidth, bool fixedHeight, bool multiline, bool invert,
                        std::vector<TextGlyphInfo>* glyphInfo = nullptr);

        TextureData* getTextureData();
        
    };
    
}

#endif /* STBText_h */
