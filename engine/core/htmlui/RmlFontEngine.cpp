// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontEngine.h"

#include "HtmlUIDependencies.h"
#include "Log.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    const uint32_t ellipsis = 0x2026;

    int toWeight(Rml::Style::FontWeight weight){
        return (weight == Rml::Style::FontWeight::Auto) ? 0 : std::min((int)weight, 1000);
    }

}

RmlFontEngine::Page::Page(): atlas(RmlFontEngine::pageSize, RmlFontEngine::pageSize, 1){
}

size_t RmlFontEngine::GlyphKeyHash::operator()(const GlyphKey& key) const{
    size_t h = std::hash<const void*>()(key.face);
    h ^= std::hash<uint32_t>()(key.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(key.rasterPixelSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

RmlFontEngine::RmlFontEngine(){
}

RmlFontEngine::~RmlFontEngine(){
    // Only reached at process exit. Rml::Shutdown already released everything; if it did
    // not run, the render managers the texture sources point to may be gone: leak them
    // instead of releasing through freed objects
    for (std::unique_ptr<Page>& page : pages){
        (void)page->source.release();
    }
}

void RmlFontEngine::Initialize(){
    FontRegistry::getBuiltInLatin();
}

void RmlFontEngine::Shutdown(){
    ReleaseFontResources();
}

bool RmlFontEngine::registerLoaded(const std::shared_ptr<FontFace>& face, const std::string& error, const std::string& family,
                                   int weight, int italic, bool fallback){
    if (!face){
        Log::error("[HTML UI] %s", error.c_str());
        return false;
    }

    FontRegistry::registerFace(face, family, weight, italic, fallback);
    retireHandles();
    return true;
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight){
    HtmlUIFileLog::recordFile(file_name);
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(file_name, face_index, &error);
    return registerLoaded(face, error, "", toWeight(weight), -1, fallback_face);
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    HtmlUIFileLog::recordFile(file_name);
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(file_name, face_index, &error);
    // the style is only meaningful with a family; without one everything comes from the file
    int italic = family.empty() ? -1 : (style == Rml::Style::FontStyle::Italic ? 1 : 0);
    return registerLoaded(face, error, family, toWeight(weight), italic, fallback_face);
}

bool RmlFontEngine::LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    std::string error;
    std::string label = "memory font" + (family.empty() ? std::string() : " '" + family + "'");
    std::shared_ptr<FontFace> face = FontFace::loadMemory(data.data(), data.size(), face_index, true, label, &error);
    int italic = family.empty() ? -1 : (style == Rml::Style::FontStyle::Italic ? 1 : 0);
    return registerLoaded(face, error, family, toWeight(weight), italic, fallback_face);
}

std::vector<std::string> RmlFontEngine::splitFamilyList(const std::string& families){
    std::vector<std::string> names;

    std::string current;
    char quote = 0;
    for (char c : families){
        if (quote){
            if (c == quote)
                quote = 0;
            else
                current += c;
        }else if (c == '"' || c == '\''){
            quote = c;
        }else if (c == ','){
            std::string name = FontRegistry::normalizeFamily(current);
            if (!name.empty())
                names.push_back(name);
            current.clear();
        }else{
            current += c;
        }
    }
    std::string name = FontRegistry::normalizeFamily(current);
    if (!name.empty())
        names.push_back(name);

    return names;
}

Rml::FontFaceHandle RmlFontEngine::GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                                     Rml::Style::FontWeight weight, int size){
    if (size <= 0)
        return 0;

    const std::vector<std::string> names = splitFamilyList(family);
    const bool italic = (style == Rml::Style::FontStyle::Italic);
    const int cssWeight = (weight == Rml::Style::FontWeight::Auto) ? 400 : std::min((int)weight, 1000);

    std::string key;
    for (const std::string& name : names){
        key += name + ",";
    }
    key += "|" + std::to_string(italic ? 1 : 0) + "|" + std::to_string(cssWeight) + "|" + std::to_string(size);

    auto it = handles.find(key);
    if (it != handles.end())
        return reinterpret_cast<Rml::FontFaceHandle>(it->second.get());

    std::unique_ptr<FaceHandle> handle(new FaceHandle());

    auto append = [&handle](const std::shared_ptr<FontFace>& face){
        if (face && std::find(handle->chain.begin(), handle->chain.end(), face) == handle->chain.end())
            handle->chain.push_back(face);
    };

    for (const std::string& name : names){
        // a missing italic or weight resolves to the nearest face of the family
        std::shared_ptr<FontFace> face = FontRegistry::find(name, cssWeight, italic);
        if (!face){
            if (warnedFamilies.insert(name).second)
                Log::warn("[HTML UI] font-family '%s' is not loaded", name.c_str());
            continue;
        }
        append(face);
    }
    // no family resolved: the default font leads (metrics, .notdef), fallbacks follow
    if (handle->chain.empty()){
        append(FontRegistry::getBuiltInLatin());
    }
    for (const std::shared_ptr<FontFace>& face : FontRegistry::getFallbackFaces()){
        append(face);
    }
    append(FontRegistry::getBuiltInLatin());
    append(FontRegistry::getBuiltInArabic());

    if (handle->chain.empty())
        return 0;

    handle->size = size;
    for (const std::shared_ptr<FontFace>& face : handle->chain){
        handle->faces.push_back(face.get());
        handle->emScales.push_back(face->scaleForEmSize((float)size));
    }

    const FontFace& primary = *handle->chain[0];
    const float s = handle->emScales[0];
    Rml::FontMetrics& metrics = handle->metrics;
    metrics.size = size;
    metrics.ascent = primary.getAscender() * s;
    metrics.descent = -primary.getDescender() * s;
    metrics.line_spacing = (primary.getAscender() - primary.getDescender() + primary.getLineGap()) * s;
    metrics.x_height = primary.getXHeight() * s;
    if (primary.hasPostTable()){
        metrics.underline_position = -primary.getUnderlinePosition() * s;
        metrics.underline_thickness = std::max(1.0f, primary.getUnderlineThickness() * s);
    }else{
        // the built-in Roboto has no post table
        metrics.underline_position = size / 12.0f;
        metrics.underline_thickness = std::max(1.0f, size / 16.0f);
    }
    metrics.has_ellipsis = false;
    for (const FontFace* face : handle->faces){
        if (face->hasCodepoint(ellipsis)){
            metrics.has_ellipsis = true;
            break;
        }
    }

    FaceHandle* raw = handle.get();
    handles[key] = std::move(handle);
    return reinterpret_cast<Rml::FontFaceHandle>(raw);
}

Rml::FontEffectsHandle RmlFontEngine::PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects){
    (void)handle;
    if (!font_effects.empty() && !warnedEffects){
        warnedEffects = true;
        Log::warn("[HTML UI] font-effect is not supported yet (phase 1)");
    }
    return 0;
}

const Rml::FontMetrics& RmlFontEngine::GetFontMetrics(Rml::FontFaceHandle handle){
    FaceHandle* face = toHandle(handle);
    return face ? face->metrics : emptyMetrics;
}

RmlFontEngine::FaceHandle* RmlFontEngine::toHandle(Rml::FontFaceHandle handle) const{
    return reinterpret_cast<FaceHandle*>(handle);
}

ShapeOptions RmlFontEngine::toShapeOptions(const Rml::TextShapingContext& context){
    ShapeOptions options;
    options.kerning = context.font_kerning != Rml::Style::FontKerning::None;
    options.letterSpacing = context.letter_spacing;
    switch (context.text_direction){
        case Rml::Style::Direction::Ltr: options.baseDirection = 1; break;
        case Rml::Style::Direction::Rtl: options.baseDirection = 2; break;
        default: options.baseDirection = 0; break;
    }
    options.language = context.language;
    return options;
}

float RmlFontEngine::measure(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options) const{
    return TextShaper::measure(handle.faces, handle.emScales, codepoints, 0, codepoints.size(), options);
}

int RmlFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                                  Rml::Character prior_character){
    FaceHandle* face = toHandle(handle);
    if (!face || string.size() == 0)
        return 0;

    const ShapeOptions options = toShapeOptions(context);
    const bool withPrior = prior_character != Rml::Character::Null && options.kerning;

    if (widthsVersion != version || widths.size() > maxWidthCacheEntries){
        widths.clear();
        widthsVersion = version;
    }

    std::string key;
    key.reserve(string.size() + options.language.size() + 48);
    key.append(reinterpret_cast<const char*>(&face), sizeof(face));
    key += options.kerning ? 'k' : '-';
    key.append(reinterpret_cast<const char*>(&options.letterSpacing), sizeof(float));
    key += (char)('0' + options.baseDirection);
    char32_t prior = withPrior ? (char32_t)prior_character : 0;
    key.append(reinterpret_cast<const char*>(&prior), sizeof(prior));
    key += options.language;
    key += '\0';
    key.append(string.begin(), string.end());

    auto it = widths.find(key);
    if (it != widths.end())
        return it->second;

    std::vector<uint32_t> codepoints;
    TextShaper::decodeUtf8(string.begin(), string.size(), codepoints);

    float width;
    if (withPrior){
        // the kerning pair across the boundary belongs to this string
        std::vector<uint32_t> priorOnly(1, (uint32_t)prior_character);
        codepoints.insert(codepoints.begin(), (uint32_t)prior_character);
        width = measure(*face, codepoints, options) - measure(*face, priorOnly, options);
    }else{
        width = measure(*face, codepoints, options);
    }

    int result = std::max(0, (int)std::lround(width));
    widths[key] = result;
    return result;
}

void RmlFontEngine::makeSource(Page& page){
    const Page* source = &page;
    page.source.reset(new Rml::CallbackTextureSource([source](const Rml::CallbackTextureInterface& textureInterface) -> bool {
        // R8 coverage -> premultiplied white (a, a, a, a); the vertex colour tints it
        const std::vector<unsigned char>& coverage = source->atlas.getPixels();
        std::vector<Rml::byte> rgba(coverage.size() * 4);
        for (size_t i = 0; i < coverage.size(); i++){
            std::memset(&rgba[i * 4], coverage[i], 4);
        }
        return textureInterface.GenerateTexture(rgba, Rml::Vector2i(pageSize, pageSize));
    }));
}

const RmlFontEngine::GlyphEntry& RmlFontEngine::getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex){
    const FontFace* fontFace = handle.faces[face];
    GlyphKey key{fontFace, glyphIndex, (int)std::lround(handle.size * rasterScale * 64.0f)};

    auto it = glyphs.find(key);
    if (it != glyphs.end())
        return it->second;

    GlyphEntry& entry = glyphs[key];

    std::vector<unsigned char> bitmap;
    int width, height, xoff, yoff;
    if (!fontFace->rasterizeGlyph(glyphIndex, handle.emScales[face] * rasterScale, bitmap, width, height, xoff, yoff))
        return entry;   // no ink

    entry.width = width;
    entry.height = height;
    entry.xoff = xoff;
    entry.yoff = yoff;

    // earlier pages first: space left there is used before a new page opens
    for (size_t i = 0; i <= pages.size() && i < maxPages; i++){
        if (i == pages.size()){
            if (width + 1 > pageSize || height + 1 > pageSize)
                break;
            pages.emplace_back(new Page());
            makeSource(*pages.back());
        }

        Page& page = *pages[i];
        int x, y;
        if (page.atlas.pack(width, height, x, y)){
            page.atlas.blit(bitmap.data(), width, height, x, y);
            page.dirty = true;
            entry.page = (int)i;
            entry.x = x;
            entry.y = y;
            return entry;
        }
    }

    if (!warnedOverflow){
        warnedOverflow = true;
        Log::error("[HTML UI] a glyph of '%s' at %d px does not fit in the font atlas (%u pages of %dx%d): it is not drawn",
                   fontFace->getLabel().c_str(), handle.size, (unsigned int)maxPages, pageSize, pageSize);
    }
    return entry;
}

void RmlFontEngine::refreshDirtyPages(){
    bool changed = false;
    for (std::unique_ptr<Page>& page : pages){
        if (!page->dirty)
            continue;
        // a new source, so RmlUi uploads the page again; the old texture is released
        // (deferred by the renderer to the end of the frame) and still holds every glyph
        // the geometry made before uses, glyphs never move
        makeSource(*page);
        page->dirty = false;
        changed = true;
    }
    if (changed)
        version++;
}

int RmlFontEngine::GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                                  Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                                  const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list){
    (void)font_effects_handle;   // font effects are phase 1

    FaceHandle* handle = toHandle(face_handle);
    if (!handle || string.size() == 0)
        return 0;

    std::vector<uint32_t> codepoints;
    TextShaper::decodeUtf8(string.begin(), string.size(), codepoints);

    std::vector<ShapedGlyph> shaped;
    TextShaper::shapeRange(handle->faces, handle->emScales, codepoints, 0, codepoints.size(), 0,
                           toShapeOptions(text_shaping_context), shaped);

    // every glyph in a page first, so the textures fetched below already hold them
    for (const ShapedGlyph& glyph : shaped){
        getGlyph(*handle, glyph.face, glyph.glyphIndex);
    }
    refreshDirtyPages();

    const Rml::ColourbPremultiplied quadColour = (opacity < 1.0f) ? colour * std::max(0.0f, opacity) : colour;
    const float scale = rasterScale;
    const float uvScale = 1.0f / (float)pageSize;

    std::vector<int> meshOfPage(pages.size(), -1);

    float pen = 0.0f;
    for (const ShapedGlyph& glyph : shaped){
        const GlyphEntry& entry = getGlyph(*handle, glyph.face, glyph.glyphIndex);

        if (entry.page >= 0){
            int& meshIndex = meshOfPage[(size_t)entry.page];
            if (meshIndex < 0){
                meshIndex = (int)mesh_list.size();
                mesh_list.emplace_back();
                mesh_list.back().texture = pages[(size_t)entry.page]->source->GetTexture(render_manager);
            }

            // snapped to physical pixels, the bitmap is drawn 1:1 on the target
            const float x = std::round((position.x + pen + glyph.xOffset) * scale + entry.xoff) / scale;
            const float y = std::round((position.y - glyph.yOffset) * scale + entry.yoff) / scale;
            const Rml::Vector2f origin(x, y);
            const Rml::Vector2f dimensions(entry.width / scale, entry.height / scale);
            const Rml::Vector2f uv0(entry.x * uvScale, entry.y * uvScale);
            const Rml::Vector2f uv1((entry.x + entry.width) * uvScale, (entry.y + entry.height) * uvScale);

            Rml::MeshUtilities::GenerateQuad(mesh_list[(size_t)meshIndex].mesh, origin, dimensions, quadColour, uv0, uv1);
        }

        pen += glyph.xAdvance;
    }

    return std::max(0, (int)std::lround(pen));
}

int RmlFontEngine::GetVersion(Rml::FontFaceHandle handle){
    (void)handle;
    return version;
}

void RmlFontEngine::clearGlyphs(){
    glyphs.clear();
    // destroying a source releases its textures through the render managers
    pages.clear();
    warnedOverflow = false;
}

void RmlFontEngine::ReleaseFontResources(){
    handles.clear();
    retiredHandles.clear();
    clearGlyphs();
    widths.clear();
    version++;
}

void RmlFontEngine::retireHandles(){
    for (auto& entry : handles){
        retiredHandles.push_back(std::move(entry.second));
    }
    handles.clear();
    widths.clear();
    version++;
}

bool RmlFontEngine::reloadFontFile(const std::string& path){
    std::string error;
    const int replaced = FontRegistry::reloadFile(path, &error);
    if (replaced == 0){
        if (!error.empty())
            Log::error("[HTML UI] %s", error.c_str());
        return false;
    }

    retireHandles();
    clearGlyphs();
    return true;
}

void RmlFontEngine::setRasterScale(float scale){
    if (!(scale > 0.0f) || std::fabs(scale - rasterScale) < 1e-6f)
        return;

    rasterScale = scale;
    clearGlyphs();
    version++;
}

float RmlFontEngine::getRasterScale() const{
    return rasterScale;
}

size_t RmlFontEngine::getPageCount() const{
    return pages.size();
}

std::vector<const FontFace*> RmlFontEngine::getChain(Rml::FontFaceHandle handle) const{
    FaceHandle* face = toHandle(handle);
    return face ? face->faces : std::vector<const FontFace*>();
}
