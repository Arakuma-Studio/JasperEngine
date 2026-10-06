// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTENGINE_H
#define RMLFONTENGINE_H

#include "Export.h"

#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>

#include <cstddef>

namespace jasperengine {

    // RmlUi's font engine on the engine text stack (FontFace, FontRegistry, TextShaper,
    // GlyphAtlas): HarfBuzz shaping, stb_truetype glyphs in append-only atlas pages.
    class JASPERENGINE_INTERNAL RmlFontEngine : public Rml::FontEngineInterface {
    public:
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

    private:
        Rml::FontMetrics emptyMetrics = {};
    };

}

#endif //RMLFONTENGINE_H
