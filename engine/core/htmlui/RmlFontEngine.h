// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTENGINE_H
#define RMLFONTENGINE_H

#include "Export.h"
#include "RmlFontEffects.h"
#include "util/GlyphAtlas.h"
#include "util/TextShaper.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    class FontFace;

    // RmlUi's font engine on the engine text stack (FontFace, FontRegistry, TextShaper,
    // GlyphAtlas): HarfBuzz shaping, stb_truetype glyphs in append-only atlas pages.
    //
    // - font-family lists are fallback chains: RmlUi 6.3 passes "A, B" as one string, split
    //   here; generic families resolve to the built-in Roboto (through FontRegistry, so a
    //   project's Roboto Bold serves sans-serif bold); every chain ends with the registered
    //   fallback faces, Roboto and Noto Sans Arabic.
    // - Sizes are CSS font-size (em size), not STBText's ascent-to-descent pixel height.
    // - Glyphs are rasterized at size * raster scale (physical pixels per document pixel)
    //   into 1024x1024 pages that only grow: a glyph never moves, so geometry generated
    //   earlier in a frame stays valid while new glyphs are added. A page with new glyphs
    //   gets a new texture source and the single global version is bumped, so RmlUi
    //   regenerates the text geometry that uses it.
    // - The raster scale is per render manager (one per HtmlUIContext: the screen context and
    //   each texture-mode context have their own pixel ratio); glyphs are keyed by their
    //   raster size, so contexts share faces and pages and keep their own glyphs.
    // - font-effect (RmlFontEffects): effects with their own texture (outline, glow, blur)
    //   get glyphs in RGBA8 pages of their own, made at document resolution (effect lengths
    //   are document pixels at every pixel ratio); shadow redraws the base glyphs offset.
    //   Back-layer effects draw first, then the text, then front-layer effects.
    // - Synthetic bold (the face found is >= 300 lighter than asked) and italic (italic asked,
    //   upright face found), as CSS font-synthesis: a dilated bitmap with a longer advance,
    //   and quads sheared by 12 degrees.
    class JASPERENGINE_INTERNAL RmlFontEngine : public Rml::FontEngineInterface {
    public:
        static constexpr int pageSize = 1024;
        static constexpr size_t maxPages = 16;
        static constexpr size_t maxWidthCacheEntries = 8192;

        RmlFontEngine();
        ~RmlFontEngine() override;

        void Initialize() override;     // registers built-ins in FontRegistry
        void Shutdown() override;       // ReleaseFontResources(), prepared effect sets
        bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override;
        bool LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;
        bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;  // copies the data
        Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                              Rml::Style::FontWeight weight, int size) override;
        // 0 for an empty list; else a set cached by the effects' fingerprints (any face)
        Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override;
        const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
        int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                           Rml::Character prior_character = Rml::Character::Null) override;
        int GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                           Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                           const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list) override;
        int GetVersion(Rml::FontFaceHandle handle) override;   // one global version
        void ReleaseFontResources() override;                  // drops handles, glyph cache, pages; faces stay registered

        // hot reload: the font file changed on disk. Its faces are parsed again (FontRegistry::reloadFile),
        // glyphs and handles are dropped so text is regenerated. False when no loaded face uses it.
        bool reloadFontFile(const std::string& path);

        // physical px per document px of the text one render manager (context) draws; a change
        // bumps the version (that context's text is regenerated), pages and glyphs stay
        void setRasterScale(Rml::RenderManager& renderManager, float scale);
        float getRasterScale(const Rml::RenderManager& renderManager) const;   // its own, else the default
        // the default for render managers without their own scale (phase 0's global scale);
        // change -> clear glyphs/pages, version++
        void setRasterScale(float scale);
        float getRasterScale() const;
        size_t getPageCount() const;        // tests: base glyph pages
        size_t getEffectPageCount() const;  // tests: RGBA8 pages of effect glyphs
        size_t getEffectSetCount() const;   // tests: prepared effect sets

        // phase-2 hook H6: whether a family is known (a generic family, a built-in or a loaded
        // face), for HTMLUI-CSS-0023 (FontRegistry::hasFamily)
        static bool hasFamily(const std::string& name);
        // tests (phase-2 golden pages, "no-text" variant): false keeps text layout and metrics but
        // generates no glyph quads (base text and font effects); a change bumps the version
        void setRenderTextForTesting(bool render);
        bool getRenderTextForTesting() const;

        // tests: the faces of a handle's chain, primary first; the "family, family" list split
        // and normalized as GetFontFaceHandle does
        std::vector<const FontFace*> getChain(Rml::FontFaceHandle handle) const;
        static std::vector<std::string> splitFamilyList(const std::string& families);

    private:
        struct FaceHandle {
            std::vector<std::shared_ptr<FontFace>> chain;
            std::vector<const FontFace*> faces;
            std::vector<float> emScales;   // document px per font unit of each face
            std::vector<float> boldStrength; // per face: synthetic bold in document px, 0 = the face has the weight
            std::vector<float> italicShear;  // per face: synthetic italic shear, 0 = none
            bool synthesizesBold = false;
            int size = 0;
            Rml::FontMetrics metrics = {};
        };

        struct GlyphKey {
            const FontFace* face;
            uint32_t glyphIndex;
            int rasterPixelSize;   // round(size * rasterScale * 64); effect glyphs: raster scale 1
            int bold;              // synthetic bold dilation in raster px, 0 = none
            size_t effect;         // RmlFontEffects::effectId, 0 = the base glyph

            bool operator==(const GlyphKey& other) const{
                return face == other.face && glyphIndex == other.glyphIndex && rasterPixelSize == other.rasterPixelSize &&
                       bold == other.bold && effect == other.effect;
            }
        };

        struct GlyphKeyHash {
            size_t operator()(const GlyphKey& key) const;
        };

        struct GlyphEntry {
            int page = -1;          // -1: nothing to draw (no ink, or it fits no page)
            int x = 0, y = 0;       // in the page, raster px
            int width = 0, height = 0;
            int xoff = 0, yoff = 0; // top-left relative to pen/baseline, raster px, y down
        };

        // R8 coverage (base glyphs) or premultiplied RGBA8 (effect glyphs)
        struct Page {
            explicit Page(int bytesPerPixel);
            GlyphAtlas atlas;
            std::unique_ptr<Rml::CallbackTextureSource> source;
            bool dirty = false;
        };
        using PageList = std::vector<std::unique_ptr<Page>>;

        static ShapeOptions toShapeOptions(const Rml::TextShapingContext& context);
        // UTF-8 to code points without the control characters (U+0000..U+001F)
        static void decodeText(Rml::StringView string, std::vector<uint32_t>& codepoints);
        FaceHandle* toHandle(Rml::FontFaceHandle handle) const;
        bool registerLoaded(const std::shared_ptr<FontFace>& face, const std::string& error, const std::string& family,
                            int weight, int italic, bool fallback);
        // RmlUi keeps handles it already has: new lookups see the new face, old handles stay valid
        void retireHandles();
        float measure(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options) const;
        // shaped glyphs with the synthetic bold advance added
        void shape(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options,
                   std::vector<ShapedGlyph>& shaped) const;
        float scaleFor(const Rml::RenderManager& renderManager) const;
        const GlyphEntry& getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale);
        const GlyphEntry& getEffectGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex,
                                         const Rml::FontEffect& effect, size_t effectId);
        // packs a bitmap into the first page of the list with room (a new page when none has);
        // false (logged once) when it fits no page
        bool place(PageList& list, int bytesPerPixel, const unsigned char* pixels, int width, int height, GlyphEntry& entry,
                   const FontFace& face, int size);
        void refreshDirtyPages();
        void makeSource(Page& page);
        void clearGlyphs();

        std::map<std::string, std::unique_ptr<FaceHandle>> handles;
        std::vector<std::unique_ptr<FaceHandle>> retiredHandles;
        std::set<std::string> warnedFamilies;

        std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> glyphs;
        PageList pages;         // base glyphs
        PageList effectPages;   // glyphs of effects with their own texture

        // prepared font-effect lists by their effect ids; kept until Shutdown, as RmlUi keeps
        // the handles it was given
        std::map<std::vector<size_t>, std::unique_ptr<RmlFontEffects::Set>> effectSets;
        std::unordered_map<const Rml::RenderManager*, float> managerScales;

        std::unordered_map<std::string, int> widths;
        int widthsVersion = 0;

        int version = 1;
        float rasterScale = 1.0f;
        bool renderText = true;
        bool warnedOverflow = false;

        Rml::FontMetrics emptyMetrics = {};
    };

}

#endif //RMLFONTENGINE_H
