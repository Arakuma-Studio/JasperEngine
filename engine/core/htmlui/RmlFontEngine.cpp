// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontEngine.h"

#include "HtmlUIDependencies.h"
#include "Log.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include "SheenBidi/SheenBidi.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
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

    // a glyph quad; synthetic italic shears it around the baseline (x += (baseline - y) * shear)
    void addQuad(Rml::Mesh& mesh, Rml::Vector2f origin, Rml::Vector2f dimensions, Rml::ColourbPremultiplied colour,
                 Rml::Vector2f uv0, Rml::Vector2f uv1, float baseline, float shear){
        Rml::MeshUtilities::GenerateQuad(mesh, origin, dimensions, colour, uv0, uv1);
        if (shear == 0.0f)
            return;

        // GenerateQuad's corners: top-left, top-right, bottom-right, bottom-left
        Rml::Vertex* vertices = &mesh.vertices[mesh.vertices.size() - 4];
        for (int i = 0; i < 4; i++){
            vertices[i].position.x += (baseline - vertices[i].position.y) * shear;
        }
    }

}

RmlFontEngine::Page::Page(int bytesPerPixel): atlas(RmlFontEngine::pageSize, RmlFontEngine::pageSize, 1, bytesPerPixel){
}

size_t RmlFontEngine::GlyphKeyHash::operator()(const GlyphKey& key) const{
    size_t h = std::hash<const void*>()(key.face);
    h ^= std::hash<uint32_t>()(key.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(key.rasterPixelSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(key.bold) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<size_t>()(key.effect) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

RmlFontEngine::RmlFontEngine(){
}

RmlFontEngine::~RmlFontEngine(){
    // Only reached at process exit. Rml::Shutdown already released everything; if it did
    // not run, the render managers the texture sources point to may be gone: leak them
    // instead of releasing through freed objects
    for (PageList* list : {&pages, &effectPages}){
        for (std::unique_ptr<Page>& page : *list){
            (void)page->source.release();
        }
    }
}

void RmlFontEngine::Initialize(){
    FontRegistry::getBuiltInLatin();
}

void RmlFontEngine::Shutdown(){
    ReleaseFontResources();
    effectSets.clear();
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

// The registry knows a font file by its path: one spelling per file (the compat layer names an
// asset "asset://ui/a.ttf", RCSS and the hot reload "ui/a.ttf"), or a reload of the file would
// not find the faces loaded under the other one.
static std::string fontFileKey(const Rml::String& file_name){
    return normalizeHtmlUIPath(file_name);
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight){
    HtmlUIFileLog::recordFile(file_name);
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(fontFileKey(file_name), face_index, &error);
    return registerLoaded(face, error, "", toWeight(weight), -1, fallback_face);
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    HtmlUIFileLog::recordFile(file_name);
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(fontFileKey(file_name), face_index, &error);
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

void RmlFontEngine::markFamilyReported(const std::string& family){
    for (const std::string& name : splitFamilyList(family))
        warnedFamilies.insert(name);
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
        // CSS font-synthesis: a face much lighter than asked is emboldened, an upright one
        // asked for italic is sheared (fallback faces too, as browsers do)
        const bool bold = cssWeight - face->getWeight() >= 300;
        handle->boldStrength.push_back(bold ? RmlFontEffects::boldStrength(size) : 0.0f);
        handle->italicShear.push_back((italic && !face->isItalic()) ? RmlFontEffects::italicShear : 0.0f);
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
    // the set does not depend on the face: glyphs are made per face when the string is
    (void)handle;

    std::vector<size_t> ids;
    ids.reserve(font_effects.size());
    for (const Rml::SharedPtr<const Rml::FontEffect>& effect : font_effects){
        if (effect)
            ids.push_back(RmlFontEffects::effectId(*effect));
    }
    if (ids.empty())
        return 0;

    std::unique_ptr<RmlFontEffects::Set>& set = effectSets[ids];
    if (!set){
        set.reset(new RmlFontEffects::Set());
        for (const Rml::SharedPtr<const Rml::FontEffect>& effect : font_effects){
            if (effect)
                set->effects.push_back(effect);
        }
        set->ids = ids;
    }

    return reinterpret_cast<Rml::FontEffectsHandle>(set.get());
}

const Rml::FontMetrics& RmlFontEngine::GetFontMetrics(Rml::FontFaceHandle handle){
    FaceHandle* face = toHandle(handle);
    if (!face)
        return emptyMetrics;
    if (!renderText){
        // the no-text test variant hides text decorations too (as the reference's
        // text-decoration-color: transparent); the layout metrics stay
        static thread_local Rml::FontMetrics noDecoration;
        noDecoration = face->metrics;
        noDecoration.underline_thickness = 0.0f;
        return noDecoration;
    }
    return face->metrics;
}

RmlFontEngine::FaceHandle* RmlFontEngine::toHandle(Rml::FontFaceHandle handle) const{
    return reinterpret_cast<FaceHandle*>(handle);
}

ShapeOptions RmlFontEngine::toShapeOptions(const Rml::TextShapingContext& context){
    ShapeOptions options;
    options.kerning = context.font_kerning != Rml::Style::FontKerning::None;
    options.letterSpacing = context.letter_spacing;
    options.wordSpacing = context.word_spacing;
    switch (context.text_direction){
        case Rml::Style::Direction::Ltr: options.baseDirection = 1; break;
        case Rml::Style::Direction::Rtl: options.baseDirection = 2; break;
        default: options.baseDirection = 0; break;
    }
    options.language = context.language;
    return options;
}

// RmlUi hands over the line feed that ends a `white-space: pre` line (ElementText's BuildToken
// keeps it in the token) and whatever control characters a document holds: they take no space and
// draw nothing, as in RmlUi's own font engine (shaped, they were the missing-glyph box)
void RmlFontEngine::decodeText(Rml::StringView string, std::vector<uint32_t>& codepoints){
    TextShaper::decodeUtf8(string.begin(), string.size(), codepoints);
    codepoints.erase(std::remove_if(codepoints.begin(), codepoints.end(), [](uint32_t codepoint){ return codepoint < 0x20; }),
                     codepoints.end());
}

void RmlFontEngine::decodeText(Rml::StringView string, const Rml::TextShapingContext& context, std::vector<uint32_t>& codepoints,
                               ShapeOptions& options){
    if (context.justification.empty()){
        decodeText(string, codepoints);
        return;
    }
    // the code points with the byte offsets they start at (a byte that does not start a valid
    // sequence is U+FFFD, as TextShaper::decodeUtf8 reads it), control characters left out
    codepoints.clear();
    std::vector<int> offsets;
    const unsigned char* text = reinterpret_cast<const unsigned char*>(string.begin());
    const size_t size = string.size();
    size_t i = 0;
    while (i < size){
        const unsigned char lead = text[i];
        uint32_t codepoint = 0xFFFD;
        size_t length = 1;
        if (lead < 0x80){
            codepoint = lead;
        }else{
            const size_t expected = (lead >= 0xF0 && lead <= 0xF4) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC2 && lead < 0xE0) ? 2 : 0;
            if (expected > 0 && i + expected <= size){
                uint32_t value = (expected == 2) ? (lead & 0x1Fu) : (expected == 3) ? (lead & 0x0Fu) : (lead & 0x07u);
                bool valid = true;
                for (size_t k = 1; k < expected; k++){
                    if ((text[i + k] & 0xC0u) != 0x80u){
                        valid = false;
                        break;
                    }
                    value = (value << 6) | (text[i + k] & 0x3Fu);
                }
                const uint32_t minimum = (expected == 2) ? 0x80u : (expected == 3) ? 0x800u : 0x10000u;
                if (valid && value >= minimum && value <= 0x10FFFFu && !(value >= 0xD800u && value <= 0xDFFFu)){
                    codepoint = value;
                    length = expected;
                }
            }
        }
        if (codepoint >= 0x20){
            codepoints.push_back(codepoint);
            offsets.push_back((int)i);
        }
        i += length;
    }

    options.justifyAdvance.assign(codepoints.size(), 0.0f);
    options.justifyOffset.assign(codepoints.size(), 0.0f);
    for (const Rml::JustificationSpacing& spacing : context.justification){
        auto it = std::lower_bound(offsets.begin(), offsets.end(), spacing.offset);
        if (it == offsets.end() || *it != spacing.offset)
            continue;
        const size_t index = (size_t)(it - offsets.begin());
        options.justifyAdvance[index] += spacing.before + spacing.after;
        options.justifyOffset[index] += spacing.before;
    }
}

void RmlFontEngine::GetBidiRuns(Rml::StringView string, const Rml::TextShapingContext& context, Rml::Vector<Rml::TextBidiRun>& runs){
    runs.clear();
    const int size = (int)string.size();
    if (size == 0)
        return;
    // the code points shapeRange sees (control characters left out, as decodeText does) and the byte each starts at
    std::vector<uint32_t> codepoints;
    std::vector<int> offsets;
    const unsigned char* text = reinterpret_cast<const unsigned char*>(string.begin());
    int i = 0;
    while (i < size){
        const unsigned char lead = text[i];
        uint32_t codepoint = 0xFFFD;
        int length = 1;
        if (lead < 0x80){
            codepoint = lead;
        }else{
            const int expected = (lead >= 0xF0 && lead <= 0xF4) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC2 && lead < 0xE0) ? 2 : 0;
            if (expected > 0 && i + expected <= size){
                uint32_t value = (expected == 2) ? (lead & 0x1Fu) : (expected == 3) ? (lead & 0x0Fu) : (lead & 0x07u);
                bool valid = true;
                for (int k = 1; k < expected; k++){
                    if ((text[i + k] & 0xC0u) != 0x80u){
                        valid = false;
                        break;
                    }
                    value = (value << 6) | (text[i + k] & 0x3Fu);
                }
                const uint32_t minimum = (expected == 2) ? 0x80u : (expected == 3) ? 0x800u : 0x10000u;
                if (valid && value >= minimum && value <= 0x10FFFFu && !(value >= 0xD800u && value <= 0xDFFFu)){
                    codepoint = value;
                    length = expected;
                }
            }
        }
        if (codepoint >= 0x20){
            codepoints.push_back(codepoint);
            offsets.push_back(i);
        }
        i += length;
    }
    const bool rtlBase = (context.text_direction == Rml::Style::Direction::Rtl);
    if (codepoints.empty()){
        runs.push_back(Rml::TextBidiRun{0, size, rtlBase});
        return;
    }
    // a run's bytes: from its first code point to the next code point after its last (the left-out control characters
    // after it with it), the first run in logical order from the string's start
    const auto byteOf = [&](size_t index){ return index == 0 ? 0 : (index >= offsets.size() ? size : offsets[index]); };

    SBCodepointSequence sequence;
    sequence.stringEncoding = SBStringEncodingUTF32;
    sequence.stringBuffer = (void*)codepoints.data();
    sequence.stringLength = (SBUInteger)codepoints.size();
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    // the paragraph's direction as shapeRange takes it: the context's, or (auto) the first strong character's
    const SBLevel baseLevel = (context.text_direction == Rml::Style::Direction::Rtl) ? 1
                            : (context.text_direction == Rml::Style::Direction::Ltr) ? 0 : SBLevelDefaultLTR;
    SBParagraphRef paragraph = algorithm ? SBAlgorithmCreateParagraph(algorithm, 0, (SBUInteger)codepoints.size(), baseLevel) : NULL;
    SBLineRef line = paragraph ? SBParagraphCreateLine(paragraph, 0, SBParagraphGetLength(paragraph)) : NULL;
    if (line){
        const SBUInteger count = SBLineGetRunCount(line);
        const SBRun* sbRuns = SBLineGetRunsPtr(line);
        for (SBUInteger r = 0; r < count; r++){
            const size_t first = (size_t)sbRuns[r].offset;
            const size_t last = first + (size_t)sbRuns[r].length;
            runs.push_back(Rml::TextBidiRun{byteOf(first), byteOf(last), (sbRuns[r].level & 1) != 0});
        }
        SBLineRelease(line);
    }
    if (paragraph)
        SBParagraphRelease(paragraph);
    if (algorithm)
        SBAlgorithmRelease(algorithm);
    // (a paragraph separator ends SheenBidi's paragraph: the rest goes on in the last run)
    int covered = 0;
    for (const Rml::TextBidiRun& run : runs)
        covered = std::max(covered, run.end);
    if (runs.empty())
        runs.push_back(Rml::TextBidiRun{0, size, rtlBase});
    else if (covered < size){
        for (Rml::TextBidiRun& run : runs){
            if (run.end == covered){
                run.end = size;
                break;
            }
        }
    }
}

float RmlFontEngine::measure(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options) const{
    return TextShaper::measure(handle.faces, handle.emScales, codepoints, 0, codepoints.size(), options);
}

void RmlFontEngine::shape(const FaceHandle& handle, const std::vector<uint32_t>& codepoints, const ShapeOptions& options,
                          std::vector<ShapedGlyph>& shaped) const{
    // a synthetic bold glyph keeps its advance: Chromium (Skia's fake bold) emboldens the outline
    // and leaves the advances alone, so bold text is as wide as the regular face's
    TextShaper::shapeRange(handle.faces, handle.emScales, codepoints, 0, codepoints.size(), 0, options, shaped);
}

float RmlFontEngine::scaleFor(const Rml::RenderManager& renderManager) const{
    auto it = managerScales.find(&renderManager);
    return (it != managerScales.end()) ? it->second : 1.0f;
}

int RmlFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                                  Rml::Character prior_character){
    FaceHandle* face = toHandle(handle);
    if (!face || string.size() == 0)
        return 0;

    ShapeOptions options = toShapeOptions(context);
    // (a control character before the string kerns with nothing)
    const bool withPrior = (char32_t)prior_character >= 0x20 && options.kerning;

    if (!context.justification.empty()){
        // a justified line (not cached): the space added around its characters
        std::vector<uint32_t> codepoints;
        decodeText(string, context, codepoints, options);
        float width = measure(*face, codepoints, options);
        if (withPrior){
            const std::vector<uint32_t> priorOnly(1, (uint32_t)prior_character);
            std::vector<uint32_t> joined(codepoints);
            joined.insert(joined.begin(), (uint32_t)prior_character);
            ShapeOptions joinedOptions = options;
            joinedOptions.justifyAdvance.insert(joinedOptions.justifyAdvance.begin(), 0.0f);
            joinedOptions.justifyOffset.insert(joinedOptions.justifyOffset.begin(), 0.0f);
            ShapeOptions priorOptions = toShapeOptions(context);
            width = measure(*face, joined, joinedOptions) - measure(*face, priorOnly, priorOptions);
        }
        return std::max(0, (int)std::lround(width));
    }

    if (widthsVersion != version || widths.size() > maxWidthCacheEntries){
        widths.clear();
        widthsVersion = version;
    }

    std::string key;
    key.reserve(string.size() + options.language.size() + 48);
    key.append(reinterpret_cast<const char*>(&face), sizeof(face));
    key += options.kerning ? 'k' : '-';
    key.append(reinterpret_cast<const char*>(&options.letterSpacing), sizeof(float));
    key.append(reinterpret_cast<const char*>(&options.wordSpacing), sizeof(float));
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
    decodeText(string, codepoints);

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
        const std::vector<unsigned char>& pixels = source->atlas.getPixels();
        // effect pages are premultiplied RGBA8 already (white, tinted by the vertex colour)
        if (source->atlas.getBytesPerPixel() == 4)
            return textureInterface.GenerateTexture(pixels, Rml::Vector2i(pageSize, pageSize));

        // R8 coverage -> premultiplied white (a, a, a, a); the vertex colour tints it
        std::vector<Rml::byte> rgba(pixels.size() * 4);
        for (size_t i = 0; i < pixels.size(); i++){
            std::memset(&rgba[i * 4], pixels[i], 4);
        }
        return textureInterface.GenerateTexture(rgba, Rml::Vector2i(pageSize, pageSize));
    }));
}

bool RmlFontEngine::place(PageList& list, int bytesPerPixel, const unsigned char* pixels, int width, int height, GlyphEntry& entry,
                          const FontFace& face, int size){
    // earlier pages first: space left there is used before a new page opens
    for (size_t i = 0; i <= list.size() && i < maxPages; i++){
        if (i == list.size()){
            if (width + 2 > pageSize || height + 2 > pageSize)
                break;
            list.emplace_back(new Page(bytesPerPixel));
            makeSource(*list.back());
        }

        Page& page = *list[i];
        int x, y;
        // The atlas adds padding on the right and bottom. Reserve the left and top too:
        // linear sampling at a page edge otherwise clamps to ink instead of transparent
        // coverage, so the same glyph changes appearance with the page's packing history.
        if (page.atlas.pack(width + 1, height + 1, x, y)){
            ++x;
            ++y;
            page.atlas.blit(pixels, width, height, x, y);
            page.dirty = true;
            entry.page = (int)i;
            entry.x = x;
            entry.y = y;
            return true;
        }
    }

    if (!warnedOverflow){
        warnedOverflow = true;
        Log::error("[HTML UI] a glyph of '%s' at %d px does not fit in the font atlas (%u pages of %dx%d): it is not drawn",
                   face.getLabel().c_str(), size, (unsigned int)maxPages, pageSize, pageSize);
    }
    return false;
}

const RmlFontEngine::GlyphEntry& RmlFontEngine::getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale){
    const FontFace* fontFace = handle.faces[face];
    const float strength = handle.boldStrength[face];
    const int bold = (strength > 0.0f) ? std::max(1, (int)std::lround(strength * scale)) : 0;
    GlyphKey key{fontFace, glyphIndex, (int)std::lround(handle.size * scale * 64.0f), bold, 0};

    auto it = glyphs.find(key);
    if (it != glyphs.end())
        return it->second;

    GlyphEntry& entry = glyphs[key];

    std::vector<unsigned char> bitmap;
    int width, height, xoff, yoff;
    if (!fontFace->rasterizeGlyph(glyphIndex, handle.emScales[face] * scale, bitmap, width, height, xoff, yoff))
        return entry;   // no ink

    if (bold > 0)
        RmlFontEffects::embolden(bitmap, width, height, yoff, bold);

    entry.width = width;
    entry.height = height;
    entry.xoff = xoff;
    entry.yoff = yoff;

    place(pages, 1, bitmap.data(), width, height, entry, *fontFace, handle.size);
    return entry;
}

const RmlFontEngine::GlyphEntry& RmlFontEngine::getEffectGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex,
                                                               const Rml::FontEffect& effect, size_t effectId){
    // Effect glyphs are made at document resolution whatever the context's raster scale: the
    // effects' lengths (outline width, blur radius) are pixels of the bitmap they are given,
    // and CSS lengths are document pixels. The base glyphs stay at the raster scale.
    const FontFace* fontFace = handle.faces[face];
    const float strength = handle.boldStrength[face];
    const int bold = (strength > 0.0f) ? std::max(1, (int)std::lround(strength)) : 0;
    GlyphKey key{fontFace, glyphIndex, handle.size * 64, bold, effectId};

    auto it = glyphs.find(key);
    if (it != glyphs.end())
        return it->second;

    GlyphEntry& entry = glyphs[key];

    std::vector<unsigned char> coverage;
    int width = 0, height = 0, xoff = 0, yoff = 0;
    if (fontFace->rasterizeGlyph(glyphIndex, handle.emScales[face], coverage, width, height, xoff, yoff) && bold > 0)
        RmlFontEffects::embolden(coverage, width, height, yoff, bold);

    const int advance = (int)std::lround(fontFace->getGlyphAdvance(glyphIndex, handle.emScales[face]));
    const Rml::FontGlyph glyph = RmlFontEffects::wrapCoverage(coverage.data(), width, height, xoff, yoff, advance);

    RmlFontEffects::Bitmap bitmap;
    if (!RmlFontEffects::generate(effect, glyph, bitmap))
        return entry;   // nothing for this glyph (no ink)

    entry.width = bitmap.width;
    entry.height = bitmap.height;
    entry.xoff = bitmap.xoff;
    entry.yoff = bitmap.yoff;

    place(effectPages, 4, bitmap.rgba.data(), bitmap.width, bitmap.height, entry, *fontFace, handle.size);
    return entry;
}

void RmlFontEngine::refreshDirtyPages(){
    bool changed = false;
    for (PageList* list : {&pages, &effectPages}){
        for (std::unique_ptr<Page>& page : *list){
            if (!page->dirty)
                continue;
            // a new source, so RmlUi uploads the page again; the old texture is released
            // (deferred by the renderer to the end of the frame) and still holds every glyph
            // the geometry made before uses, glyphs never move
            makeSource(*page);
            page->dirty = false;
            changed = true;
        }
    }
    if (changed)
        version++;
}

int RmlFontEngine::GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                                  Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                                  const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list){
    FaceHandle* handle = toHandle(face_handle);
    if (!handle || string.size() == 0)
        return 0;

    const RmlFontEffects::Set* effects = reinterpret_cast<const RmlFontEffects::Set*>(font_effects_handle);
    const float scale = scaleFor(render_manager);

    std::vector<uint32_t> codepoints;
    ShapeOptions options = toShapeOptions(text_shaping_context);
    decodeText(string, text_shaping_context, codepoints, options);

    std::vector<ShapedGlyph> shaped;
    shape(*handle, codepoints, options, shaped);

    if (!renderText){
        // the no-text test variant: the same advance, no quads
        float pen = 0.0f;
        for (const ShapedGlyph& glyph : shaped)
            pen += glyph.xAdvance;
        return std::max(0, (int)std::lround(pen));
    }

    // every glyph in a page first, so the textures fetched below already hold them
    for (const ShapedGlyph& glyph : shaped){
        getGlyph(*handle, glyph.face, glyph.glyphIndex, scale);
        if (effects){
            for (size_t i = 0; i < effects->effects.size(); i++){
                if (effects->effects[i]->HasUniqueTexture())
                    getEffectGlyph(*handle, glyph.face, glyph.glyphIndex, *effects->effects[i], effects->ids[i]);
            }
        }
    }
    refreshDirtyPages();

    const float uvScale = 1.0f / (float)pageSize;

    // One layer: the base text (effect null) or one effect, each with its own meshes (one per
    // page) so the mesh list draws in layer order
    std::vector<int> meshOfPage;
    auto emitLayer = [&](const Rml::FontEffect* effect, size_t effectId){
        const bool unique = effect && effect->HasUniqueTexture();
        PageList& list = unique ? effectPages : pages;
        meshOfPage.assign(list.size(), -1);

        Rml::ColourbPremultiplied quadColour;
        if (effect){
            quadColour = effect->GetColour().ToPremultiplied(std::max(0.0f, std::min(opacity, 1.0f)));
        }else{
            quadColour = (opacity < 1.0f) ? colour * std::max(0.0f, opacity) : colour;
        }

        float pen = 0.0f;
        for (const ShapedGlyph& glyph : shaped){
            const float penX = position.x + pen + glyph.xOffset;
            const float baseline = position.y - glyph.yOffset;
            pen += glyph.xAdvance;

            const GlyphEntry& entry = unique ? getEffectGlyph(*handle, glyph.face, glyph.glyphIndex, *effect, effectId)
                                             : getGlyph(*handle, glyph.face, glyph.glyphIndex, scale);
            if (entry.page < 0)
                continue;

            Rml::Vector2f origin;
            Rml::Vector2f dimensions;
            if (unique){
                // document pixels, snapped as RmlUi's FontFaceLayer does
                origin = Rml::Vector2f(std::round(penX) + entry.xoff, std::round(baseline) + entry.yoff);
                dimensions = Rml::Vector2f((float)entry.width, (float)entry.height);
            }else{
                // snapped to physical pixels, the bitmap is drawn 1:1 on the target
                origin = Rml::Vector2f(std::round(penX * scale + entry.xoff) / scale, std::round(baseline * scale + entry.yoff) / scale);
                dimensions = Rml::Vector2f(entry.width / scale, entry.height / scale);
                if (effect){
                    // shared-texture effect (shadow): the base glyph moved
                    Rml::Vector2i offset;
                    const Rml::FontGlyph metrics = RmlFontEffects::wrapCoverage(nullptr, entry.width, entry.height, entry.xoff, entry.yoff,
                                                                                (int)std::lround(glyph.xAdvance));
                    if (!RmlFontEffects::offset(*effect, metrics, offset))
                        continue;
                    origin += Rml::Vector2f(offset);
                }
            }

            int& meshIndex = meshOfPage[(size_t)entry.page];
            if (meshIndex < 0){
                meshIndex = (int)mesh_list.size();
                mesh_list.emplace_back();
                mesh_list.back().texture = list[(size_t)entry.page]->source->GetTexture(render_manager);
            }

            const Rml::Vector2f uv0(entry.x * uvScale, entry.y * uvScale);
            const Rml::Vector2f uv1((entry.x + entry.width) * uvScale, (entry.y + entry.height) * uvScale);
            addQuad(mesh_list[(size_t)meshIndex].mesh, origin, dimensions, quadColour, uv0, uv1, baseline,
                    handle->italicShear[glyph.face]);
        }
        return pen;
    };

    // back-layer effects in list order, the text, front-layer effects in list order
    if (effects){
        for (size_t i = 0; i < effects->effects.size(); i++){
            if (effects->effects[i]->GetLayer() == Rml::FontEffect::Layer::Back)
                emitLayer(effects->effects[i].get(), effects->ids[i]);
        }
    }
    const float width = emitLayer(nullptr, 0);
    if (effects){
        for (size_t i = 0; i < effects->effects.size(); i++){
            if (effects->effects[i]->GetLayer() == Rml::FontEffect::Layer::Front)
                emitLayer(effects->effects[i].get(), effects->ids[i]);
        }
    }

    return std::max(0, (int)std::lround(width));
}

bool RmlFontEngine::hasFamily(const std::string& name){
    return FontRegistry::hasFamily(name);
}

void RmlFontEngine::setRenderTextForTesting(bool render){
    if (renderText == render)
        return;
    renderText = render;
    version++;
}

bool RmlFontEngine::getRenderTextForTesting() const{
    return renderText;
}

int RmlFontEngine::GetVersion(Rml::FontFaceHandle handle){
    (void)handle;
    return version;
}

void RmlFontEngine::clearGlyphs(){
    glyphs.clear();
    // destroying a source releases its textures through the render managers
    pages.clear();
    effectPages.clear();
    warnedOverflow = false;
}

void RmlFontEngine::ReleaseFontResources(){
    handles.clear();
    retiredHandles.clear();
    clearGlyphs();
    widths.clear();
    // RmlUi::ReleaseRenderManagers comes through here: a later manager may reuse an address
    managerScales.clear();
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
    const int replaced = FontRegistry::reloadFile(normalizeHtmlUIPath(path), &error);
    if (replaced == 0){
        if (!error.empty())
            Log::error("[HTML UI] %s", error.c_str());
        return false;
    }

    retireHandles();
    clearGlyphs();
    return true;
}

void RmlFontEngine::setRasterScale(Rml::RenderManager& renderManager, float scale){
    if (!(scale > 0.0f))
        return;

    const float previous = scaleFor(renderManager);
    managerScales[&renderManager] = scale;
    // that context's text was made at the old scale; glyphs of both scales stay cached
    if (std::fabs(scale - previous) >= 1e-6f)
        version++;
}

float RmlFontEngine::getRasterScale(const Rml::RenderManager& renderManager) const{
    return scaleFor(renderManager);
}
