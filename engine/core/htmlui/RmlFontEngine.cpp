// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontEngine.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/StringUtilities.h>

using namespace jasperengine;

void RmlFontEngine::Initialize(){
    // HTMLUI_SKELETON: p0-font
}

void RmlFontEngine::Shutdown(){
    // HTMLUI_SKELETON: p0-font
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight){
    // HTMLUI_SKELETON: p0-font
    (void)file_name;
    (void)face_index;
    (void)fallback_face;
    (void)weight;
    return false;
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    // HTMLUI_SKELETON: p0-font
    (void)file_name;
    (void)face_index;
    (void)family;
    (void)style;
    (void)weight;
    (void)fallback_face;
    return false;
}

bool RmlFontEngine::LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    // HTMLUI_SKELETON: p0-font
    (void)data;
    (void)face_index;
    (void)family;
    (void)style;
    (void)weight;
    (void)fallback_face;
    return false;
}

Rml::FontFaceHandle RmlFontEngine::GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                                     Rml::Style::FontWeight weight, int size){
    // HTMLUI_SKELETON: p0-font
    (void)family;
    (void)style;
    (void)weight;
    (void)size;
    return 0;
}

Rml::FontEffectsHandle RmlFontEngine::PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects){
    // HTMLUI_SKELETON: p0-font
    (void)handle;
    (void)font_effects;
    return 0;
}

const Rml::FontMetrics& RmlFontEngine::GetFontMetrics(Rml::FontFaceHandle handle){
    // HTMLUI_SKELETON: p0-font
    (void)handle;
    return emptyMetrics;
}

int RmlFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                                  Rml::Character prior_character){
    // HTMLUI_SKELETON: p0-font
    (void)handle;
    (void)string;
    (void)context;
    (void)prior_character;
    return 0;
}

int RmlFontEngine::GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                                  Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                                  const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list){
    // HTMLUI_SKELETON: p0-font
    (void)render_manager;
    (void)face_handle;
    (void)font_effects_handle;
    (void)string;
    (void)position;
    (void)colour;
    (void)opacity;
    (void)text_shaping_context;
    (void)mesh_list;
    return 0;
}

int RmlFontEngine::GetVersion(Rml::FontFaceHandle handle){
    // HTMLUI_SKELETON: p0-font
    (void)handle;
    return 0;
}

void RmlFontEngine::ReleaseFontResources(){
    // HTMLUI_SKELETON: p0-font
}

void RmlFontEngine::setRasterScale(float scale){
    // HTMLUI_SKELETON: p0-font
    (void)scale;
}

float RmlFontEngine::getRasterScale() const{
    // HTMLUI_SKELETON: p0-font
    return 1.0f;
}

size_t RmlFontEngine::getPageCount() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}
