// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPFONT_H
#define PIXELART_BITMAPFONT_H

// A bitmap (pixel) font (docs/specs/S51-pixel-fonts.md): glyph images at their native size, whole
// pixel metrics, packed once into its own atlas. Built from a .bitmapfont (JSON settings over a
// grid sheet, a BMFont .fnt or a pixel TrueType font), a .fnt used directly, a cooked .jfont or
// the built-in `builtin:pixel`. The scale is in the quads (BitmapText), never in the atlas.

#include "Export.h"
#include "texture/TextureData.h"

#include <cstdint>
#include <map>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine::pixelart {

    // the family's typed error (ledger "Agent behaviour"): what, why, the fix, the file key
    struct JASPERENGINE_API FontError {
        std::string kind;
        std::string message;
        std::string fix;
        std::string field;
    };

    struct BitmapGlyph {
        uint16_t page = 0, x = 0, y = 0, w = 0, h = 0;   // the atlas rect; w or h 0: no ink (a space)
        int16_t offsetX = 0, offsetY = 0;                 // the image's top-left from the pen at the line top, y down
        int16_t advance = 0;                              // pen advance before spacing and kerning
    };

    // R6: what an import measured, the same for the same file
    struct JASPERENGINE_API BitmapFontReport {
        int glyphs = 0;
        int cellsUsed = 0;
        int cellsEmpty = 0;
        int nativeSize = 0;
        int lineHeight = 0;
        int baseline = 0;
        int cellWidth = 0;
        int cellHeight = 0;
        bool colored = false;
        float partialAlphaShare = 0.0f;         // texels 0 < a < 255 over texels a > 0 (after alpha_cutoff)
        std::vector<uint32_t> missingAscii;     // U+0020-U+007E the font lacks
        std::vector<uint32_t> emptyGlyphs;      // glyphs without ink other than space
        int overridesApplied = 0;               // glyphs entries and kerning pairs of the file
        int kerningPairs = 0;
        std::vector<std::string> detections;    // what was detected and how (never silent, R2)
        std::vector<FontError> errors;
        std::vector<FontError> warnings;
    };

    // the settings of a .bitmapfont (R2-R5): every key optional except the source
    struct JASPERENGINE_API BitmapFontSettings {
        enum class Source { GRID, BMFONT, TRUETYPE };
        enum class Advance { AUTO, MONOSPACE, FILE, FIXED };

        int version = 1;
        Source source = Source::GRID;
        std::string file;                       // grid image, .fnt or .ttf/.otf: relative to the .bitmapfont
        // grid
        int cellWidth = 0, cellHeight = 0;      // 0: from the file name, else detected
        int marginX = 0, marginY = 0;
        int gapX = 0, gapY = 0;
        std::string chars;                      // "" = the source's default ("ascii" grid, "latin1" TrueType)
        std::vector<std::string> rows;          // one string per sheet row (grid), instead of chars
        // TrueType
        int pixelSize = 0;                      // 0: detected
        float threshold = 0.5f;
        // metrics (R5)
        std::optional<Advance> advance;         // unset: AUTO, FILE for BMFont
        int advanceValue = 0;                   // Advance::FIXED
        bool trimBoth = false;
        int spacing = 1;
        std::optional<int> spaceWidth;
        std::optional<int> lineHeight;
        std::optional<int> baseline;
        std::string fallback;                   // "" = "?" when present, else a hollow box
        int colored = -1;                       // -1 auto, 0 monochrome, 1 coloured
        int alphaCutoff = 0;
        struct GlyphOverride {
            std::optional<int> advance;
            int offsetX = 0, offsetY = 0;
        };
        std::map<uint32_t, GlyphOverride> glyphs;
        struct Kern { uint32_t left = 0, right = 0; int amount = 0; };
        std::vector<Kern> kerning;
        std::vector<std::string> fallbackFonts;
    };

    enum class FontKind { VECTOR, BITMAP };

    inline constexpr const char* kBuiltinPixelFont = "builtin:pixel";
    inline constexpr int kBitmapFontVersion = 1;
    inline constexpr int kMaxAtlasPage = 2048;   // R18: under GLES3/WebGL2's guaranteed 4096

    // by extension (.bitmapfont, .fnt, .jfont) and builtin: names (D1)
    JASPERENGINE_API FontKind fontKindOf(const std::string& path);
    JASPERENGINE_API bool isBitmapFontPath(const std::string& path);

    struct BitmapFontPage {
        int width = 0, height = 0;
        int channels = 1;                       // 1 (R8 monochrome) or 4 (RGBA8 coloured)
        std::vector<uint8_t> pixels;
        std::unique_ptr<TextureData> texture;   // points into pixels, without owning them
    };

    // one glyph image before packing (the parsers' output)
    struct RawGlyph {
        uint32_t codepoint = 0;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;              // width * height * 4, straight alpha
        int offsetX = 0, offsetY = 0;           // the image's top-left from the pen at the line top
        int fileAdvance = -1;                   // BMFont xadvance, TrueType advance; -1 none
        int cell = -1;                          // grid cell index
    };

    struct RawFont {
        std::vector<RawGlyph> glyphs;           // in source order
        int cellWidth = 0, cellHeight = 0;      // grid cells; the em box elsewhere
        int cellsTotal = 0;
        int nativeSize = 0;
        int lineHeight = -1, baseline = -1;     // from the source, -1 = derive
        int spaceAdvance = -1;
        bool fileAdvances = false;              // BMFont: "file" is the default advance
        std::vector<BitmapFontSettings::Kern> kerning;
        std::vector<std::string> detections;
    };

    class JASPERENGINE_API BitmapFont {
    public:
        BitmapFont() = default;
        BitmapFont(const BitmapFont&) = delete;
        BitmapFont& operator=(const BitmapFont&) = delete;

        // .bitmapfont, .fnt, .jfont or builtin:pixel, read through the engine's files (Data::open).
        // A shipped game reads <name>.jfont next to a source when it exists (R34). nullptr on an
        // error, which `report` describes (typed)
        static std::shared_ptr<BitmapFont> load(const std::string& path, BitmapFontReport* report = nullptr);
        // the same from bytes already read; `path` resolves the files they name
        static std::shared_ptr<BitmapFont> loadSettings(const std::string& path, const std::string& json, BitmapFontReport* report);
        static std::shared_ptr<BitmapFont> loadBMFont(const std::string& path, const std::vector<uint8_t>& bytes,
                                                      const BitmapFontSettings* settings, BitmapFontReport* report);
        static std::shared_ptr<BitmapFont> loadCooked(const std::vector<uint8_t>& bytes, BitmapFontReport* report);
        static std::shared_ptr<BitmapFont> builtinPixel();

        // applies the settings' metrics to a parsed source and packs the atlas (R5, R6, R11)
        static std::shared_ptr<BitmapFont> build(RawFont raw, const BitmapFontSettings& settings, BitmapFontReport* report);

        // one font with `primary`'s metrics and every glyph of `others` it lacks (same native
        // size): a Text's chain in one atlas (R14)
        static std::shared_ptr<BitmapFont> merged(const std::shared_ptr<BitmapFont>& primary,
                                                  const std::vector<std::shared_ptr<BitmapFont>>& others);

        // the glyph drawn for a codepoint: its own, else the fallback glyph (never null)
        const BitmapGlyph* glyph(uint32_t codepoint) const;
        const BitmapGlyph* findGlyph(uint32_t codepoint) const;     // nullptr when missing
        bool hasGlyph(uint32_t codepoint) const;
        int kerning(uint32_t left, uint32_t right) const;
        // advance + spacing + kerning(prev, cur) of each codepoint of a line, whole font pixels
        int advanceOf(uint32_t codepoint) const;
        int measure(std::u32string_view line) const;                // a line's width, no trailing spacing

        int getNativeSize() const { return nativeSize; }
        int getLineHeight() const { return lineHeight; }
        int getBaseline() const { return baseline; }
        int getSpacing() const { return spacing; }
        int getSpaceWidth() const { return spaceWidth; }
        int getXHeight() const;
        bool isColored() const { return colored; }
        size_t getGlyphCount() const { return glyphs.size(); }
        const std::vector<uint32_t>& getCodepoints() const { return codepoints; }
        const std::unordered_map<uint64_t, int>& getKerningTable() const { return kerningTable; }
        const BitmapGlyph& getFallbackGlyph() const { return fallbackGlyph; }

        const std::vector<BitmapFontPage>& getPages() const { return pages; }
        TextureData* getPageTexture(size_t page) const;
        // the glyph's image as RGBA8 (monochrome: white with its alpha), w * h * 4
        void copyGlyphRgba(const BitmapGlyph& glyph, std::vector<uint8_t>& out) const;

        // the cooked form (R34): magic JFNT, version, metrics, glyphs, kerning, deflated pages
        std::vector<uint8_t> toCooked() const;

        // where the font was loaded from and the files it read (hot reload, S05)
        const std::string& getPath() const { return path; }
        const std::vector<std::string>& getDependencies() const { return dependencies; }
        const BitmapFontReport& getReport() const { return report; }
        // unique per loaded font (caches key on it: an address can be reused)
        uint64_t getSerial() const { return serial; }

    private:
        friend class BitmapFontBuilder;

        static uint64_t nextSerial();

        uint64_t serial = nextSerial();
        std::string path;
        std::vector<std::string> dependencies;
        BitmapFontReport report;

        int nativeSize = 0;
        int lineHeight = 0;
        int baseline = 0;
        int spacing = 1;
        int spaceWidth = 1;
        bool colored = false;

        std::unordered_map<uint32_t, BitmapGlyph> glyphs;
        std::vector<uint32_t> codepoints;                   // sorted
        std::unordered_map<uint64_t, int> kerningTable;     // (left << 32) | right
        BitmapGlyph fallbackGlyph;
        std::vector<BitmapFontPage> pages;
        // ASCII's glyphs by codepoint (null: missing): a Text's layout looks them up per character
        std::array<const BitmapGlyph*, 128> asciiGlyphs{};

        void makeTextures();
        void indexGlyphs();
    };

    // R5 file keys: parse a .bitmapfont document (lenient: unknown keys are the caller's to keep)
    JASPERENGINE_API bool parseBitmapFontSettings(const std::string& json, BitmapFontSettings& settings, std::vector<FontError>& errors);
    // a .bitmapfont document with sorted glyphs and kerning, the known keys in their documented
    // order, then unknown keys sorted (R33); `json` is the whole document to keep its unknown keys
    JASPERENGINE_API std::string canonicalBitmapFontJson(const std::string& json);
    // a path relative to the .bitmapfont / .fnt that names it, as the engine opens files
    JASPERENGINE_API std::string resolveFontRelative(const std::string& fontPath, const std::string& relative);
    // UTF-8 to codepoints, the named sets "ascii", "latin1", "latin_ext" expanded
    JASPERENGINE_API std::vector<uint32_t> expandFontChars(const std::string& chars, const char* defaultSet);
    JASPERENGINE_API std::string codepointToUtf8(uint32_t codepoint);

}

#endif //PIXELART_BITMAPFONT_H
