// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTENGINE_H
#define RMLFONTENGINE_H

#include "Export.h"
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
    class JASPERENGINE_INTERNAL RmlFontEngine : public Rml::FontEngineInterface {
    public:
        static constexpr int pageSize = 1024;
        static constexpr size_t maxPages = 16;
        static constexpr size_t maxWidthCacheEntries = 8192;

        RmlFontEngine();
        ~RmlFontEngine() override;

        void Initialize() override;     // registers built-ins in FontRegistry
        void Shutdown() override;       // ReleaseFontResources()
        bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override;
        bool LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;
        bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                          Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override;  // copies the data
        Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                              Rml::Style::FontWeight weight, int size) override;
        Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override; // 0, warns once (phase 1)
        const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
        int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                           Rml::Character prior_character = Rml::Character::Null) override;
        int GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                           Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                           const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list) override;
        int GetVersion(Rml::FontFaceHandle handle) override;   // one global version
        void ReleaseFontResources() override;                  // drops handles, glyph cache, pages; faces stay registered

        void setRasterScale(float scale);   // physical px per document px; change -> clear glyphs/pages, version++
        float getRasterScale() const;
        size_t getPageCount() const;        // tests

        // tests: the faces of a handle's chain, primary first; the "family, family" list split
        // and normalized as GetFontFaceHandle does
        std::vector<const FontFace*> getChain(Rml::FontFaceHandle handle) const;
        static std::vector<std::string> splitFamilyList(const std::string& families);

    private:
        struct FaceHandle {
            std::vector<std::shared_ptr<FontFace>> chain;
            std::vector<const FontFace*> faces;
            std::vector<float> emScales;   // document px per font unit of each face
            int size = 0;
            Rml::FontMetrics metrics = {};
        };

        struct GlyphKey {
            const FontFace* face;
            uint32_t glyphIndex;
            int rasterPixelSize;   // round(size * rasterScale * 64)

            bool operator==(const GlyphKey& other) const{
                return face == other.face && glyphIndex == other.glyphIndex && rasterPixelSize == other.rasterPixelSize;
            }
        };

        struct GlyphKeyHash {
            size_t operator()(const GlyphKey& key) const;
        };

        struct GlyphEntry {
            int page = -1;          // -1: nothing to draw (no ink, or it fits no page)
            int x = 0, y = 0;       // in the page, physical px
            int width = 0, height = 0;
            int xoff = 0, yoff = 0; // top-left relative to pen/baseline, physical px, y down
        };

        struct Page {
            Page();
            GlyphAtlas atlas;
            std::unique_ptr<Rml::CallbackTextureSource> source;
            bool dirty = false;
        };

        static ShapeOptions toShapeOptions(const Rml::TextShapingContext& context);
        FaceHandle* toHandle(Rml::FontFaceHandle handle) const;
        bool registerLoaded(const std::shared_ptr<FontFace>& face, const std::string& error, const std::string& family,
                            int weight, int italic, bool fallback);
        // RmlUi keeps handles it already has: new lookups see the new face, old handles stay valid
        void retireHandles();
        float measure(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options) const;
        const GlyphEntry& getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex);
        void refreshDirtyPages();
        void makeSource(Page& page);
        void clearGlyphs();

        std::map<std::string, std::unique_ptr<FaceHandle>> handles;
        std::vector<std::unique_ptr<FaceHandle>> retiredHandles;
        std::set<std::string> warnedFamilies;

        std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> glyphs;
        std::vector<std::unique_ptr<Page>> pages;

        std::unordered_map<std::string, int> widths;
        int widthsVersion = 0;

        int version = 1;
        float rasterScale = 1.0f;
        bool warnedEffects = false;
        bool warnedOverflow = false;

        Rml::FontMetrics emptyMetrics = {};
    };

}

#endif //RMLFONTENGINE_H
