// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontEngine.h"
#include "pixelart/BitmapFont.h"
#include "htmlui/HtmlTextEffects.h"

#include "HtmlUIDependencies.h"
#include "HtmlUITexture.h"
#include "HtmlUIClock.h"
#include "Engine.h"
#include "effects/EffectRuntime.h"
#include "Log.h"
#include "RmlFontMsdf.h"
#include "RmlRenderer.h"
#include "compat/DiagnosticCodes.h"
#include "jobs/JobSystem.h"
#include "render/SystemRender.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include "SheenBidi/SheenBidi.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace {

    const uint32_t ellipsis = 0x2026;

    constexpr size_t MiB = 1024 * 1024;

    std::atomic<bool> transformAwareText{true};
    std::atomic<bool> gpuTextEffects{true};
    std::atomic<bool> synchronousDistanceGlyphs{false};

    // A deterministic clock (play's fixed_fps, a fixed UI or effect time, the tests' switch): the
    // text of a frame depends on the frames before it only, not on wall time: distance glyphs
    // are waited for, and the settle budget counts glyphs, not milliseconds
    bool deterministicText(){
        return synchronousDistanceGlyphs || Engine::getFixedDeltatime() > 0.0 || HtmlUIClock::getFixedTime().has_value() ||
               htmlui::EffectClock::isFixed();
    }

    // GPU text effects are the text layer of D1: off with transform-aware text, as before S16
    bool gpuEffects(){
        return gpuTextEffects && transformAwareText;
    }
    std::atomic<size_t> baseBudgetMiB{0};
    std::atomic<size_t> distanceBudgetMiB{0};

    // R13: mobile and the web keep half the desktop's atlas
    size_t defaultBaseBudgetMiB(){
    #if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        return 8;
    #else
        return 16;
    #endif
    }

    // a transform-aware raster size (quarter device px of the em) never equals a legacy one
    constexpr int quarterPixelKey = 1 << 30;

    // more than this many evictions within the window: the budget is too small (HTMLUI-GEN-0007)
    constexpr size_t evictionWarnCount = 4;
    constexpr uint64_t evictionWarnFrames = 120;

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

// A distance glyph made on the background lane: the face is kept alive by the job
struct RmlFontEngine::DistanceJob {
    DistanceKey key;
    std::shared_ptr<FontFace> face;
    jobs::Task<RmlDistanceGlyph> task;
    std::set<FaceHandle*> waiting;      // handles that asked for it: bumped once it is placed
};

RmlFontEngine::Page::Page(RmlFontEngine& owner, PageKind pageKind):
    engine(owner), kind(pageKind),
    atlas(std::make_shared<GlyphAtlas>(RmlFontEngine::pageSize, RmlFontEngine::pageSize, 1, pageKind == PageKind::BASE ? 1 : 4)){
}

size_t RmlFontEngine::Page::bytes() const{
    if (atlas->getWidth() == 0)
        return 0;
    return (size_t)pageSize * (size_t)pageSize * (kind == PageKind::BASE ? 1u : 4u);
}

uint64_t RmlFontEngine::Page::lastUsed() const{
    return std::max(lastUse, *drawFrame);
}

size_t RmlFontEngine::DistanceKeyHash::operator()(const DistanceKey& key) const{
    size_t h = std::hash<const void*>()(key.face);
    h ^= std::hash<uint32_t>()(key.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(key.bold) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
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
    for (PageList* list : {&pages, &effectPages, &distancePages}){
        for (std::unique_ptr<Page>& page : *list){
            (void)page->source.release();
            for (auto& variant : page->variants)
                (void)variant.second.release();
        }
    }
    // the jobs in flight finish (they hold their faces), their results are dropped
    for (std::unique_ptr<DistanceJob>& job : distanceJobs)
        job->task.wait();
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
    if (pixelart::isBitmapFontPath(file_name))
        return loadBitmapFace(file_name, "");
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(fontFileKey(file_name), face_index, &error);
    return registerLoaded(face, error, "", toWeight(weight), -1, fallback_face);
}

bool RmlFontEngine::LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& family,
                                 Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face){
    HtmlUIFileLog::recordFile(file_name);
    if (pixelart::isBitmapFontPath(file_name))
        return loadBitmapFace(file_name, family);
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

    // a bitmap family leads: a bitmap face, never mixed with vector fallbacks (S51 R14, R15)
    if (!names.empty()){
        if (std::shared_ptr<pixelart::BitmapFont> bitmap = bitmapFamilyFont(names[0])){
            std::unique_ptr<FaceHandle> made = makeBitmapHandle(bitmap, size);
            FaceHandle* raw = made.get();
            handles[key] = std::move(made);
            return reinterpret_cast<Rml::FontFaceHandle>(raw);
        }
    }

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
    return (it != managerScales.end()) ? it->second : rasterScale;
}

int RmlFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context,
                                  Rml::Character prior_character){
    FaceHandle* face = toHandle(handle);
    if (!face || string.size() == 0)
        return 0;
    if (face->bitmap)
        return bitmapStringWidth(*face, string, context);

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

    if (widthsEpoch != epoch || widths.size() > maxWidthCacheEntries){
        widths.clear();
        widthsEpoch = epoch;
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

//-------------------------------------------------------------------------------------------
// pages: sources, budget, eviction (S16 R12, R13)
//-------------------------------------------------------------------------------------------

void RmlFontEngine::makeSource(Page& page){
    Page* source = &page;
    page.textures.clear();
    // a new source: its variants were made from the old one's texture
    page.variants.clear();
    page.source.reset(new Rml::CallbackTextureSource([source](const Rml::CallbackTextureInterface& textureInterface) -> bool {
        const std::vector<unsigned char>& pixels = source->atlas->getPixels();
        Rml::RenderManager& manager = textureInterface.GetRenderManager();
        if (RmlRenderer* renderer = RmlRenderer::forRenderManager(&manager)){
            // the engine's renderer: R8 coverage with its white block, MTSDF, or RGBA8 as they are,
            // uploaded (again when glyphs are added) before the frame draws
            RmlRenderer::AtlasTexture desc;
            desc.drawFrame = source->drawFrame;
            desc.atlas = source->atlas;
            desc.generation = source->generation;
            switch (source->kind){
            case PageKind::BASE:
                desc.sample = HtmlUITexture::Sample::COVERAGE;
                desc.whiteX = source->whiteX;
                desc.whiteY = source->whiteY;
                break;
            case PageKind::EFFECT:
                desc.sample = HtmlUITexture::Sample::RGBA;
                break;
            case PageKind::DISTANCE:
                desc.sample = HtmlUITexture::Sample::MSDF;
                desc.distanceRange = RmlFontMsdf::distanceRange;
                break;
            }
            const Rml::TextureHandle handle = renderer->createAtlasTexture(pageSize, pageSize, desc);
            if (!handle)
                return false;
            source->textures[&manager] = handle;
            textureInterface.SetTextureHandle(handle, Rml::Vector2i(pageSize, pageSize));
            return true;
        }

        // another render interface: effect and distance pages as they are; R8 coverage as
        // premultiplied white (a, a, a, a), which the vertex colour tints. Its texture does not
        // follow the page: new glyphs make a new source
        source->foreign = true;
        if (source->atlas->getBytesPerPixel() == 4)
            return textureInterface.GenerateTexture(pixels, Rml::Vector2i(pageSize, pageSize));
        std::vector<Rml::byte> rgba(pixels.size() * 4);
        for (size_t i = 0; i < pixels.size(); i++){
            std::memset(&rgba[i * 4], pixels[i], 4);
        }
        return textureInterface.GenerateTexture(rgba, Rml::Vector2i(pageSize, pageSize));
    }));
}

void RmlFontEngine::reserveWhiteBlock(Page& page){
    // in the page's last rows, below every shelf and a row of padding: the glyphs are packed
    // where they were before S16 (a glyph drawn under a transform samples its edge rows, and
    // a block among them moved them: AC1), whether the frame batches or not
    if (page.kind != PageKind::BASE)
        return;
    const int height = (int)page.atlas->getHeight();
    if (height <= whiteBlockSize + 1)
        return;
    page.atlas->reserveBottom(whiteBlockSize + 1);
    const int x = 0, y = height - whiteBlockSize;
    const std::vector<unsigned char> white((size_t)(whiteBlockSize * whiteBlockSize), 255);
    page.atlas->blit(white.data(), whiteBlockSize, whiteBlockSize, x, y);
    (*page.generation)++;
    page.whiteX = x;
    page.whiteY = y;
}

RmlFontEngine::PageList& RmlFontEngine::pagesOf(PageKind kind){
    switch (kind){
    case PageKind::BASE: return pages;
    case PageKind::EFFECT: return effectPages;
    case PageKind::DISTANCE: return distancePages;
    }
    return pages;
}

size_t RmlFontEngine::budgetBytes(PageKind kind) const{
    // base and CPU effect pages share the base budget; at least one page of the kind fits
    const size_t budget = (kind == PageKind::DISTANCE) ? getDistanceBudgetBytes() : getBaseBudgetBytes();
    const size_t page = (size_t)pageSize * (size_t)pageSize * (kind == PageKind::BASE ? 1u : 4u);
    return std::max(budget, page);
}

size_t RmlFontEngine::usedBytes(PageKind kind) const{
    size_t used = 0;
    const bool distance = kind == PageKind::DISTANCE;
    for (const PageList* list : {&pages, &effectPages, &distancePages}){
        for (const std::unique_ptr<Page>& page : *list){
            if ((page->kind == PageKind::DISTANCE) == distance)
                used += page->bytes();
        }
    }
    return used;
}

size_t RmlFontEngine::getAtlasBytes() const{
    return usedBytes(PageKind::BASE) + usedBytes(PageKind::DISTANCE);
}

void RmlFontEngine::bump(FaceHandle* handle){
    if (handle)
        handle->version++;
}

void RmlFontEngine::bumpEpoch(){
    epoch++;
}

void RmlFontEngine::markUsed(Page& page, FaceHandle* user){
    page.lastUse = SystemRender::getFrameCount();
    if (user)
        page.users.insert(user);
}

void RmlFontEngine::clearPage(Page& page, bool bump){
    const PageList& list = pagesOf(page.kind);
    int index = -1;
    for (size_t i = 0; i < list.size(); i++){
        if (list[i].get() == &page)
            index = (int)i;
    }
    // its glyphs go, and the text that drew them is generated again
    if (page.kind == PageKind::DISTANCE){
        for (auto it = distanceGlyphs.begin(); it != distanceGlyphs.end();){
            if (it->second.ready && it->second.page == index)
                it = distanceGlyphs.erase(it);
            else
                ++it;
        }
    }else{
        for (auto it = glyphs.begin(); it != glyphs.end();){
            if (it->second.kind == page.kind && it->second.page == index)
                it = glyphs.erase(it);
            else
                ++it;
        }
    }
    if (bump){
        for (FaceHandle* user : page.users)
            this->bump(user);
    }
    page.users.clear();
    // parked: no pixels, no texture (its index stays: entries name pages by index)
    page.source.reset();
    page.textures.clear();
    page.variants.clear();
    page.atlas->reset(0, 0);
    (*page.generation)++;
    page.foreign = false;
    page.whiteX = page.whiteY = -1;
    page.dirty = false;
}

void RmlFontEngine::noteEviction(){
    evictions++;
    const uint64_t frame = SystemRender::getFrameCount();
    if (FrameCounters* counters = countersOf(currentManager))
        counters->atlasEvictions++;
    evictionFrames.push_back(frame);
    while (!evictionFrames.empty() && evictionFrames.front() + evictionWarnFrames < frame)
        evictionFrames.erase(evictionFrames.begin());
    // the pages keep being cleared for glyphs they held: the budget is too small for the text drawn
    if (evictionFrames.size() > evictionWarnCount && warnedBudget.insert(currentManager).second){
        Log::warn("[HTML UI] glyph atlas budget too small: %u atlas pages cleared within %u frames (%u + %u MiB); raise the project's "
                  "glyph atlas budget [%s]",
                  (unsigned int)evictionFrames.size(), (unsigned int)evictionWarnFrames, (unsigned int)(getBaseBudgetBytes() / MiB),
                  (unsigned int)(getDistanceBudgetBytes() / MiB), htmlui::compat::codes::GlyphAtlasBudget.id().c_str());
    }
}

bool RmlFontEngine::evictPage(PageKind kind){
    // the least recently used page of the budget, drawn neither in this frame nor the last one
    const uint64_t frame = SystemRender::getFrameCount();
    const bool distance = kind == PageKind::DISTANCE;
    Page* oldest = nullptr;
    for (PageList* list : {&pages, &effectPages, &distancePages}){
        for (std::unique_ptr<Page>& page : *list){
            if ((page->kind == PageKind::DISTANCE) != distance || page->bytes() == 0)
                continue;
            if (page->lastUsed() + 1 >= frame)
                continue;
            if (!oldest || page->lastUsed() < oldest->lastUsed())
                oldest = page.get();
        }
    }
    if (!oldest)
        return false;
    clearPage(*oldest, true);
    noteEviction();
    return true;
}

bool RmlFontEngine::place(PageKind kind, const unsigned char* pixels, int width, int height, int& pageIndex, int& x, int& y,
                          const std::string& label, int size){
    PageList& list = pagesOf(kind);
    auto packInto = [&](size_t i) -> bool {
        Page& page = *list[i];
        if (page.bytes() == 0 || !page.atlas->pack(width, height, x, y))
            return false;
        page.atlas->blit(pixels, width, height, x, y);
        (*page.generation)++;
        page.dirty = true;
        page.lastUse = SystemRender::getFrameCount();
        pageIndex = (int)i;
        return true;
    };
    // Glyphs fill the newest page; with room in the budget, then the space left in the others,
    // then a new page. With the budget full, a new page in place of the least recently used one
    // (unused in this frame and the last, R13); else, when a page drawn last frame is unused in
    // this one, the glyph waits a frame for it (deferred); and only then the space left anywhere.
    // Filling the newest page keeps a document's glyphs on few pages, so the pages of what is no
    // longer drawn become free to clear, instead of every page holding a little of everything.
    const uint64_t frame = SystemRender::getFrameCount();
    const size_t pageBytes = (size_t)pageSize * (size_t)pageSize * (kind == PageKind::BASE ? 1u : 4u);
    const bool fitsAPage = width + 2 <= pageSize && height + 2 <= pageSize;
    placeDeferred = false;
    size_t newest = list.size();
    for (size_t i = 0; i < list.size(); i++){
        if (list[i]->bytes() > 0 && (newest == list.size() || list[i]->opened > list[newest]->opened))
            newest = i;
    }
    if (newest < list.size() && packInto(newest))
        return true;
    auto anyPage = [&](){
        for (size_t i = 0; i < list.size(); i++){
            if (i != newest && packInto(i))
                return true;
        }
        return false;
    };
    auto openPage = [&]() -> bool {
        if (usedBytes(kind) + pageBytes > budgetBytes(kind))
            return false;
        size_t slot = list.size();
        for (size_t i = 0; i < list.size(); i++){
            if (list[i]->bytes() == 0){
                slot = i;
                break;
            }
        }
        if (slot == list.size())
            list.emplace_back(new Page(*this, kind));
        else
            list[slot]->atlas->reset(pageSize, pageSize);
        list[slot]->opened = ++pagesOpened;
        // transform-aware text draws glyphs at fractional positions and scales, where filtering
        // reads a texel past their edges: an empty one on every side wherever they are packed
        // (at the page's top or left edge clamping read the glyph's own edge row instead, so the
        // same text drew otherwise once its page was cleared and refilled). Off, the glyphs are
        // packed where they were before S16 (AC1).
        if (transformAwareText)
            list[slot]->atlas->reserveLeading(1);
        reserveWhiteBlock(*list[slot]);
        makeSource(*list[slot]);
        return packInto(slot);
    };
    if (usedBytes(kind) + pageBytes <= budgetBytes(kind)){
        if (anyPage())
            return true;
        if (fitsAPage && openPage())
            return true;
    }else if (fitsAPage){
        bool evicted = false;
        while (usedBytes(kind) + pageBytes > budgetBytes(kind) && evictPage(kind))
            evicted = true;
        if (evicted && openPage())
            return true;
        // (three frames in a row at most: text that keeps the pages it needs in use does not fit)
        const bool streakEnds = deferredStreak >= 3 && deferredFrame + 1 >= frame && deferredFrame != frame;
        for (const std::unique_ptr<Page>& page : list){
            if (!streakEnds && page->bytes() > 0 && page->lastUsed() < frame)
                placeDeferred = true;
        }
        if (placeDeferred)
            return false;
        if (anyPage())
            return true;
    }

    if (!warnedOverflow){
        warnedOverflow = true;
        Log::error("[HTML UI] a glyph of '%s' at %d px does not fit in the font atlas (%u MiB, pages of %dx%d): it is not drawn",
                   label.c_str(), size, (unsigned int)(budgetBytes(kind) / MiB), pageSize, pageSize);
    }
    return false;
}

//-------------------------------------------------------------------------------------------
// glyphs
//-------------------------------------------------------------------------------------------

float RmlFontEngine::rasterScaleOf(const Rml::RenderManager* renderManager, const FaceHandle& handle, const Rml::TextShapingContext& context,
                                   int& rasterKey) const{
    if (transformAwareText && context.raster_scale > 0.0f && handle.size > 0){
        // R14: keyed by quarter device pixels of the em, rasterized at exactly that size
        const int quarters = std::max(1, (int)std::lround((float)handle.size * context.raster_scale * 4.0f));
        rasterKey = quarters | quarterPixelKey;
        return (float)quarters / (4.0f * (float)handle.size);
    }
    const float scale = renderManager ? scaleFor(*renderManager) : rasterScale;
    rasterKey = (int)std::lround(handle.size * scale * 64.0f);
    return scale;
}

const RmlFontEngine::GlyphEntry* RmlFontEngine::findGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale,
                                                          int rasterKey) const{
    const float strength = handle.boldStrength[face];
    const int bold = (strength > 0.0f) ? std::max(1, (int)std::lround(strength * scale)) : 0;
    auto it = glyphs.find(GlyphKey{handle.faces[face], glyphIndex, rasterKey, bold, 0});
    return it == glyphs.end() ? nullptr : &it->second;
}

const RmlFontEngine::GlyphEntry& RmlFontEngine::getGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex, float scale,
                                                         int rasterKey){
    const FontFace* fontFace = handle.faces[face];
    const float strength = handle.boldStrength[face];
    const int bold = (strength > 0.0f) ? std::max(1, (int)std::lround(strength * scale)) : 0;
    GlyphKey key{fontFace, glyphIndex, rasterKey, bold, 0};

    auto it = glyphs.find(key);
    if (it != glyphs.end())
        return it->second;

    GlyphEntry& entry = glyphs[key];
    entry.kind = PageKind::BASE;
    FrameCounters* counters = countersOf(currentManager);
    if (counters)
        counters->glyphsRasterized++;
    const auto start = std::chrono::steady_clock::now();
    struct RasterTime {
        FrameCounters* counters;
        std::chrono::steady_clock::time_point start;
        ~RasterTime(){
            if (counters)
                counters->glyphRasterMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    } rasterTime{counters, start};

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

    int page = -1, x = 0, y = 0;
    if (place(PageKind::BASE, bitmap.data(), width, height, page, x, y, fontFace->getLabel(), handle.size)){
        entry.page = page;
        entry.x = x;
        entry.y = y;
    }else if (placeDeferred){
        glyphs.erase(key);
        deferGlyph(handle);
        static const GlyphEntry none;
        return none;
    }
    // (place may have cleared pages: the entry is looked up again)
    return glyphs[key];
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
    entry.kind = PageKind::EFFECT;

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

    int page = -1, x = 0, y = 0;
    if (place(PageKind::EFFECT, bitmap.rgba.data(), bitmap.width, bitmap.height, page, x, y, fontFace->getLabel(), handle.size)){
        entry.page = page;
        entry.x = x;
        entry.y = y;
    }else if (placeDeferred){
        glyphs.erase(key);
        deferGlyph(handle);
        static const GlyphEntry none;
        return none;
    }
    return glyphs[key];
}

void RmlFontEngine::refreshDirtyPages(){
    for (PageList* list : {&pages, &effectPages, &distancePages}){
        for (std::unique_ptr<Page>& page : *list){
            if (!page->dirty)
                continue;
            page->dirty = false;
            // the engine's renderer uploads the page's new generation into the same textures
            if (!page->foreign)
                continue;
            // another render interface: a new source, so RmlUi uploads the page again; the old
            // texture is released (deferred to the end of the frame) and still holds every glyph
            // the geometry made before uses, glyphs never move. That geometry is made again: the
            // handles that drew from the page get a new version
            makeSource(*page);
            page->foreign = false;
            for (FaceHandle* user : page->users)
                bump(user);
        }
    }
}

//-------------------------------------------------------------------------------------------
// distance glyphs (R16)
//-------------------------------------------------------------------------------------------

RmlFontEngine::DistanceKey RmlFontEngine::distanceKey(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex) const{
    // synthetic bold as a share of the em, in 1/1024 steps
    const float boldEm = (handle.size > 0) ? handle.boldStrength[face] / (float)handle.size : 0.0f;
    return DistanceKey{handle.faces[face], glyphIndex, (int)std::lround(boldEm * 1024.0f)};
}

const RmlFontEngine::DistanceEntry& RmlFontEngine::requestDistanceGlyph(const FaceHandle& handle, unsigned int face, uint32_t glyphIndex){
    const DistanceKey key = distanceKey(handle, face, glyphIndex);
    auto it = distanceGlyphs.find(key);
    if (it != distanceGlyphs.end()){
        if (!it->second.ready){
            for (std::unique_ptr<DistanceJob>& job : distanceJobs){
                if (job->key == key)
                    job->waiting.insert(const_cast<FaceHandle*>(&handle));
            }
        }
        return it->second;
    }
    DistanceEntry& entry = distanceGlyphs[key];
    // made on the background lane, never in the frame (R16)
    std::unique_ptr<DistanceJob> job(new DistanceJob());
    job->key = key;
    job->face = handle.chain[face];
    job->waiting.insert(const_cast<FaceHandle*>(&handle));
    const std::shared_ptr<FontFace> fontFace = job->face;
    const float boldEm = (float)key.bold / 1024.0f;
    job->task = jobs::submit([fontFace, glyphIndex, boldEm](){
        RmlDistanceGlyph glyph;
        RmlFontMsdf::generate(*fontFace, glyphIndex, boldEm, glyph);
        return glyph;
    }, jobs::SubmitOptions{jobs::Lane::Background, jobs::GateClass::Default, "htmlui-msdf-glyph"});
    distanceJobs.push_back(std::move(job));
    if (deterministicText()){
        // (a deterministic clock, the tests) at once: waited for and placed, the asking text drawn
        // from it in this frame
        collectDistanceGlyphs(true);
        return distanceGlyphs[key];
    }
    return entry;
}

void RmlFontEngine::setSynchronousDistanceGlyphsForTesting(bool synchronous){
    synchronousDistanceGlyphs = synchronous;
}

void RmlFontEngine::collectDistanceGlyphs(bool wait){
    for (size_t i = 0; i < distanceJobs.size();){
        DistanceJob& job = *distanceJobs[i];
        if (wait)
            job.task.wait();
        if (!job.task.isReady()){
            i++;
            continue;
        }
        auto it = distanceGlyphs.find(job.key);
        if (it != distanceGlyphs.end()){
            DistanceEntry& entry = it->second;
            entry.ready = true;
            const RmlDistanceGlyph& glyph = job.task.get();
            if (glyph.width > 0 && glyph.height > 0){
                int page = -1, x = 0, y = 0;
                if (place(PageKind::DISTANCE, glyph.rgba.data(), glyph.width, glyph.height, page, x, y, job.face->getLabel(),
                          (int)RmlFontMsdf::emTexels)){
                    // (place may have cleared a page and its entries: looked up again)
                    DistanceEntry& placed = distanceGlyphs[job.key];
                    placed.ready = true;
                    placed.page = page;
                    placed.x = x;
                    placed.y = y;
                    placed.width = glyph.width;
                    placed.height = glyph.height;
                    placed.left = glyph.left;
                    placed.bottom = glyph.bottom;
                    placed.right = glyph.right;
                    placed.top = glyph.top;
                }
            }
            // the text that asked draws again, now from the distance glyph (made at once, it already does)
            if (!deterministicText()){
                for (FaceHandle* handle : job.waiting)
                    bump(handle);
            }
        }
        distanceJobs.erase(distanceJobs.begin() + (long)i);
    }
}

size_t RmlFontEngine::getPendingDistanceGlyphCount() const{
    size_t pending = 0;
    for (const std::unique_ptr<DistanceJob>& job : distanceJobs){
        if (!job->task.isReady())
            pending++;
    }
    return pending;
}

void RmlFontEngine::finishDistanceGlyphsForTesting(){
    collectDistanceGlyphs(true);
    refreshDirtyPages();
}

bool RmlFontEngine::distanceEffectOf(const FaceHandle& handle, const Rml::FontEffect& effect, std::array<float, 3>& parameters) const{
    Rml::FontEffect::DistanceEffect distance;
    if (!gpuEffects() || !effect.GetDistanceEffect(distance))
        return false;
    // within the distance the glyphs hold: 0.3 em (R17)
    const float reach = distance.dilation + 3.0f * distance.sigma;
    if (reach > RmlFontMsdf::reachEm * (float)handle.size)
        return false;
    parameters = {distance.sigma > 0.0f ? 2.0f : 1.0f, distance.dilation, distance.sigma};
    return true;
}

Rml::Texture RmlFontEngine::variantTexture(Page& page, const std::array<float, 3>& parameters, Rml::RenderManager& renderManager){
    std::unique_ptr<Rml::CallbackTextureSource>& variant = page.variants[parameters];
    if (!variant){
        Page* base = &page;
        const std::array<float, 3> effect = parameters;
        variant.reset(new Rml::CallbackTextureSource([base, effect](const Rml::CallbackTextureInterface& textureInterface) -> bool {
            Rml::RenderManager& manager = textureInterface.GetRenderManager();
            RmlRenderer* renderer = RmlRenderer::forRenderManager(&manager);
            if (!renderer || !base->source)
                return false;
            // the page's own texture first (made now if it is not yet), then a variant on its image
            base->source->GetTexture(manager).GetDimensions();
            auto it = base->textures.find(&manager);
            if (it == base->textures.end())
                return false;
            const Rml::TextureHandle handle = renderer->createTextureVariant(it->second, effect.data(), base->drawFrame);
            if (!handle)
                return false;
            textureInterface.SetTextureHandle(handle, Rml::Vector2i(pageSize, pageSize));
            return true;
        }));
    }
    return variant->GetTexture(renderManager);
}

//-------------------------------------------------------------------------------------------
// strings
//-------------------------------------------------------------------------------------------

int RmlFontEngine::GenerateString(Rml::RenderManager& render_manager, Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle font_effects_handle,
                                  Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                                  const Rml::TextShapingContext& text_shaping_context, Rml::TexturedMeshList& mesh_list){
    FaceHandle* handle = toHandle(face_handle);
    if (!handle || string.size() == 0)
        return 0;
    currentManager = &render_manager;
    const RmlFontEffects::Set* effects = reinterpret_cast<const RmlFontEffects::Set*>(font_effects_handle);
    if (handle->bitmap)
        return generateBitmapString(render_manager, *handle, effects, string, position, colour, opacity, text_shaping_context, mesh_list);
    collectDistanceGlyphs(false);

    int rasterKey = 0;
    const float scale = rasterScaleOf(&render_manager, *handle, text_shaping_context, rasterKey);
    // the bitmap maps 1:1 onto device pixels at the element's own scale: quads are sized and
    // snapped by it, not by the quarter-pixel size the glyph was made at (R14)
    const float drawScale = (transformAwareText && text_shaping_context.raster_scale > 0.0f) ? text_shaping_context.raster_scale : scale;
    // distance glyphs and GPU effects need the engine's renderer (another render interface sees bitmaps)
    const bool gpu = RmlRenderer::forRenderManager(&render_manager) != nullptr;

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

    // the base text from distance glyphs: asked for (distance_field) or a GPU-blurred shadow
    const bool shadowBlur = gpu && gpuEffects() && text_shaping_context.shadow_blur > 0.0f;
    const bool distanceText = gpu && ((transformAwareText && text_shaping_context.distance_field) || shadowBlur);
    // each effect's way: from distance glyphs (GPU) when they are all there and within reach
    std::vector<std::array<float, 3>> effectParameters;
    std::vector<bool> effectOnGpu;
    bool distanceReady = true;
    if (gpu && (distanceText || (effects && gpuEffects()))){
        for (const ShapedGlyph& glyph : shaped){
            if (!requestDistanceGlyph(*handle, glyph.face, glyph.glyphIndex).ready)
                distanceReady = false;
        }
    }
    if (effects){
        for (size_t i = 0; i < effects->effects.size(); i++){
            std::array<float, 3> parameters = {};
            const bool unique = effects->effects[i]->HasUniqueTexture();
            const bool onGpu = unique && gpu && distanceReady && distanceEffectOf(*handle, *effects->effects[i], parameters);
            effectParameters.push_back(parameters);
            effectOnGpu.push_back(onGpu);
            if (unique && !onGpu){
                if (FrameCounters* counters = countersOf(&render_manager))
                    counters->textEffectFallbacks++;
            }
        }
    }

    // every glyph in a page first, so the textures fetched below already hold them; the pages
    // they are on are in use from now (a later glyph may not clear them)
    const uint64_t frame = SystemRender::getFrameCount();
    auto keep = [&](const GlyphEntry& entry){
        PageList& list = pagesOf(entry.kind);
        if (entry.page >= 0 && entry.page < (int)list.size())
            list[(size_t)entry.page]->lastUse = frame;
    };
    for (const ShapedGlyph& glyph : shaped){
        if (!distanceText)
            keep(getGlyph(*handle, glyph.face, glyph.glyphIndex, scale, rasterKey));
        if (effects){
            for (size_t i = 0; i < effects->effects.size(); i++){
                if (effects->effects[i]->HasUniqueTexture() && !effectOnGpu[i])
                    keep(getEffectGlyph(*handle, glyph.face, glyph.glyphIndex, *effects->effects[i], effects->ids[i]));
            }
        }
    }
    refreshDirtyPages();

    const float uvScale = 1.0f / (float)pageSize;

    // a text effect (HtmlTextEffects): each visible glyph's quad and colour, which every layer
    // of it then follows (the shadow and outline layers move with the glyph and take its alpha)
    struct GlyphEffect {
        bool active = false;
        Rml::Vector2f boxMin, boxMax;
        Rml::Vector2f corners[4];   // top-left, top-right, bottom-right, bottom-left
        Rml::ColourbPremultiplied colour;
        float alpha = 1.0f;         // the glyph's alpha over the text's
    };
    std::vector<GlyphEffect> glyphEffects;
    if (const htmlui::HtmlTextEffectGlyphs* fx = static_cast<const htmlui::HtmlTextEffectGlyphs*>(text_shaping_context.text_effect)){
        if (fx->effect && fx->count > 0){
            // the effect's index of each code point: visible characters (not white space) counted
            auto isSpace = [](uint32_t c){
                return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200B);
            };
            std::vector<int> indexOf(codepoints.size(), -1);
            int visible = 0;
            for (size_t c = 0; c < codepoints.size(); c++){
                if (!isSpace(codepoints[c]))
                    indexOf[c] = text_shaping_context.glyph_index_base + visible++;
            }

            const float alphaBase = colour.alpha / 255.0f;
            const Vector4 base = alphaBase > 0.0f
                ? Vector4(colour.red / 255.0f / alphaBase, colour.green / 255.0f / alphaBase, colour.blue / 255.0f / alphaBase, alphaBase)
                : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            const float layerOpacity = std::max(0.0f, std::min(opacity, 1.0f));

            glyphEffects.resize(shaped.size());
            float pen = 0.0f;
            for (size_t g = 0; g < shaped.size(); g++){
                const ShapedGlyph& glyph = shaped[g];
                const float penX = position.x + pen + glyph.xOffset;
                const float baseline = position.y - glyph.yOffset;
                pen += glyph.xAdvance;
                const int index = glyph.cluster < indexOf.size() ? indexOf[glyph.cluster] : -1;
                const GlyphEntry& entry = getGlyph(*handle, glyph.face, glyph.glyphIndex, scale, rasterKey);
                if (index < 0 || entry.page < 0 || entry.width <= 0 || entry.height <= 0)
                    continue;

                GlyphEffect& effect = glyphEffects[g];
                effect.active = true;
                effect.boxMin = Rml::Vector2f(std::round(penX * scale + entry.xoff) / scale, std::round(baseline * scale + entry.yoff) / scale);
                effect.boxMax = effect.boxMin + Rml::Vector2f(entry.width / scale, entry.height / scale);

                TextEffectGlyphState state;
                state.corners[0] = Vector2(effect.boxMin.x, effect.boxMin.y);
                state.corners[1] = Vector2(effect.boxMax.x, effect.boxMin.y);
                state.corners[2] = Vector2(effect.boxMax.x, effect.boxMax.y);
                state.corners[3] = Vector2(effect.boxMin.x, effect.boxMax.y);
                state.color = base;
                for (size_t l = 0; l < fx->effect->layers.size(); l++){
                    TextEffectLayer layer = fx->effect->layers[l];
                    // a px amplitude in em of this text
                    if (l < fx->effect->amplitudeInPx.size() && fx->effect->amplitudeInPx[l])
                        layer.amplitude /= fx->em;
                    const float time = TextEffects::isOneShot(layer.type) ? fx->cycleTime : fx->time;
                    TextEffects::applyLayer(layer, index, fx->count, time, fx->em, fx->reducedMotion, state, fx->characters, fx->characters);
                }
                for (int c = 0; c < 4; c++)
                    effect.corners[c] = Rml::Vector2f(state.corners[c].x, state.corners[c].y);
                const float a = std::max(0.0f, std::min(state.color.w, 1.0f)) * layerOpacity;
                auto channel = [&](float v){ return (Rml::byte)std::lround(std::max(0.0f, std::min(v, 1.0f)) * a * 255.0f); };
                effect.colour = Rml::ColourbPremultiplied(channel(state.color.x), channel(state.color.y), channel(state.color.z),
                                                          (Rml::byte)std::lround(a * 255.0f));
                effect.alpha = (base.w > 0.0f) ? std::max(0.0f, std::min(state.color.w / base.w, 1.0f)) : 0.0f;
            }
        }
    }
    // the last quad of a mesh moved as its glyph's effect moved the glyph's box
    auto applyGlyphEffect = [&](Rml::Mesh& mesh, size_t g, bool baseLayer){
        if (g >= glyphEffects.size() || !glyphEffects[g].active || mesh.vertices.size() < 4)
            return;
        const GlyphEffect& effect = glyphEffects[g];
        const Rml::Vector2f size = effect.boxMax - effect.boxMin;
        Rml::Vertex* vertices = &mesh.vertices[mesh.vertices.size() - 4];
        for (int i = 0; i < 4; i++){
            const float u = size.x != 0.0f ? (vertices[i].position.x - effect.boxMin.x) / size.x : 0.0f;
            const float v = size.y != 0.0f ? (vertices[i].position.y - effect.boxMin.y) / size.y : 0.0f;
            const Rml::Vector2f top = effect.corners[0] + (effect.corners[1] - effect.corners[0]) * u;
            const Rml::Vector2f bottom = effect.corners[3] + (effect.corners[2] - effect.corners[3]) * u;
            vertices[i].position = top + (bottom - top) * v;
            if (baseLayer){
                vertices[i].colour = effect.colour;
            }else{
                Rml::ColourbPremultiplied& c = vertices[i].colour;
                c = Rml::ColourbPremultiplied((Rml::byte)std::lround(c.red * effect.alpha), (Rml::byte)std::lround(c.green * effect.alpha),
                                              (Rml::byte)std::lround(c.blue * effect.alpha), (Rml::byte)std::lround(c.alpha * effect.alpha));
            }
        }
    };

    // One layer: the base text (effect null) or one effect, each with its own meshes (one per
    // page) so the mesh list draws in layer order
    std::vector<int> meshOfPage;
    auto emitLayer = [&](const Rml::FontEffect* effect, size_t effectIndex){
        const bool unique = effect && effect->HasUniqueTexture();
        const bool effectGpu = unique && effectOnGpu[effectIndex];
        const bool fromDistance = effectGpu || (!effect && distanceText) || (effect && !unique && distanceText);
        const PageKind kind = fromDistance ? PageKind::DISTANCE : (unique ? PageKind::EFFECT : PageKind::BASE);
        PageList& list = pagesOf(kind);
        meshOfPage.assign(list.size(), -1);
        // the distance effect drawn: an effect's, a blurred shadow, or the glyph itself
        std::array<float, 3> parameters = {0.0f, 0.0f, 0.0f};
        Rml::Vector2f effectOffset(0.0f, 0.0f);
        if (effectGpu){
            parameters = effectParameters[effectIndex];
            Rml::FontEffect::DistanceEffect distance;
            effect->GetDistanceEffect(distance);
            effectOffset = distance.offset;
        }else if (!effect && shadowBlur){
            parameters = {2.0f, 0.0f, 0.5f * text_shaping_context.shadow_blur};
        }
        const bool variant = parameters[0] != 0.0f;

        Rml::ColourbPremultiplied quadColour;
        if (effect){
            quadColour = effect->GetColour().ToPremultiplied(std::max(0.0f, std::min(opacity, 1.0f)));
        }else{
            quadColour = (opacity < 1.0f) ? colour * std::max(0.0f, opacity) : colour;
        }

        float pen = 0.0f;
        for (size_t glyphNumber = 0; glyphNumber < shaped.size(); glyphNumber++){
            const ShapedGlyph& glyph = shaped[glyphNumber];
            const float penX = position.x + pen + glyph.xOffset;
            const float baseline = position.y - glyph.yOffset;
            pen += glyph.xAdvance;

            Rml::Vector2f origin;
            Rml::Vector2f dimensions;
            Rml::Vector2f uv0, uv1;
            int pageIndex = -1;
            if (fromDistance){
                const DistanceEntry& entry = requestDistanceGlyph(*handle, glyph.face, glyph.glyphIndex);
                if (!entry.ready || entry.page < 0)
                    continue;
                // the bitmap's rectangle in font units, at the handle's size (document px)
                const float em = handle->emScales[glyph.face];
                origin = Rml::Vector2f(penX + entry.left * em, baseline - entry.top * em) + effectOffset;
                dimensions = Rml::Vector2f((entry.right - entry.left) * em, (entry.top - entry.bottom) * em);
                uv0 = Rml::Vector2f(entry.x * uvScale, entry.y * uvScale);
                uv1 = Rml::Vector2f((entry.x + entry.width) * uvScale, (entry.y + entry.height) * uvScale);
                pageIndex = entry.page;
                if (effect && !unique){
                    // shared-texture effect (shadow) of distance text: the glyph moved
                    Rml::Vector2i offset;
                    const Rml::FontGlyph metrics = RmlFontEffects::wrapCoverage(nullptr, 1, 1, 0, 0, (int)std::lround(glyph.xAdvance));
                    if (!RmlFontEffects::offset(*effect, metrics, offset))
                        continue;
                    origin += Rml::Vector2f(offset);
                }
            }else{
                const GlyphEntry& entry = unique ? getEffectGlyph(*handle, glyph.face, glyph.glyphIndex, *effect, effects->ids[effectIndex])
                                                 : getGlyph(*handle, glyph.face, glyph.glyphIndex, scale, rasterKey);
                if (entry.page < 0)
                    continue;
                if (unique){
                    // document pixels, snapped as RmlUi's FontFaceLayer does
                    origin = Rml::Vector2f(std::round(penX) + entry.xoff, std::round(baseline) + entry.yoff);
                    dimensions = Rml::Vector2f((float)entry.width, (float)entry.height);
                }else{
                    // snapped to physical pixels, the bitmap is drawn 1:1 on the target
                    origin = Rml::Vector2f(std::round(penX * drawScale + entry.xoff) / drawScale,
                                           std::round(baseline * drawScale + entry.yoff) / drawScale);
                    dimensions = Rml::Vector2f(entry.width / drawScale, entry.height / drawScale);
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
                uv0 = Rml::Vector2f(entry.x * uvScale, entry.y * uvScale);
                uv1 = Rml::Vector2f((entry.x + entry.width) * uvScale, (entry.y + entry.height) * uvScale);
                pageIndex = entry.page;
            }
            if (pageIndex < 0 || pageIndex >= (int)list.size() || !list[(size_t)pageIndex]->source)
                continue;

            Page& page = *list[(size_t)pageIndex];
            markUsed(page, handle);
            int& meshIndex = meshOfPage[(size_t)pageIndex];
            if (meshIndex < 0){
                meshIndex = (int)mesh_list.size();
                mesh_list.emplace_back();
                mesh_list.back().texture = variant ? variantTexture(page, parameters, render_manager) : page.source->GetTexture(render_manager);
            }
            addQuad(mesh_list[(size_t)meshIndex].mesh, origin, dimensions, quadColour, uv0, uv1, baseline, handle->italicShear[glyph.face]);
            // (a text-shadow keeps its colour: only the glyph's alpha over the text's applies)
            applyGlyphEffect(mesh_list[(size_t)meshIndex].mesh, glyphNumber, effect == nullptr && !text_shaping_context.text_effect_shadow);
        }
        return pen;
    };

    // back-layer effects in list order, the text, front-layer effects in list order
    if (effects){
        for (size_t i = 0; i < effects->effects.size(); i++){
            if (effects->effects[i]->GetLayer() == Rml::FontEffect::Layer::Back)
                emitLayer(effects->effects[i].get(), i);
        }
    }
    const float width = emitLayer(nullptr, 0);
    if (effects){
        for (size_t i = 0; i < effects->effects.size(); i++){
            if (effects->effects[i]->GetLayer() == Rml::FontEffect::Layer::Front)
                emitLayer(effects->effects[i].get(), i);
        }
    }

    return std::max(0, (int)std::lround(width));
}

//-------------------------------------------------------------------------------------------
// fork S16-F3
//-------------------------------------------------------------------------------------------

float RmlFontEngine::GetBaseRasterScale(Rml::RenderManager& render_manager){
    // (ElementText asks first in its OnRender: the context whose text the next calls prepare)
    currentManager = &render_manager;
    if (!transformAwareText || !RmlRenderer::forRenderManager(&render_manager))
        return 0.0f;
    return scaleFor(render_manager);
}

bool RmlFontEngine::PrepareGlyphs(Rml::FontFaceHandle face_handle, Rml::StringView string, const Rml::TextShapingContext& context){
    FaceHandle* handle = toHandle(face_handle);
    if (!handle || string.size() == 0 || !renderText || handle->bitmap)
        return true;
    // the frame's budget (R15, R22): 256 glyphs, 2 ms; the rest wait for the next frame
    const uint64_t frame = SystemRender::getFrameCount();
    if (settleFrame != frame){
        settleFrame = frame;
        settleGlyphs = 0;
        settleMs = 0.0;
    }
    int rasterKey = 0;
    const float scale = rasterScaleOf(currentManager, *handle, context, rasterKey);
    std::vector<uint32_t> codepoints;
    ShapeOptions options = toShapeOptions(context);
    decodeText(string, context, codepoints, options);
    std::vector<ShapedGlyph> shaped;
    shape(*handle, codepoints, options, shaped);
    bool ready = true;
    for (const ShapedGlyph& glyph : shaped){
        if (findGlyph(*handle, glyph.face, glyph.glyphIndex, scale, rasterKey))
            continue;
        if (settleGlyphs >= settleGlyphBudget || (settleMs >= settleTimeBudgetMs && !deterministicText())){
            ready = false;
            break;
        }
        const auto start = std::chrono::steady_clock::now();
        getGlyph(*handle, glyph.face, glyph.glyphIndex, scale, rasterKey);
        settleMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        settleGlyphs++;
    }
    return ready;
}

bool RmlFontEngine::PrepareDistanceGlyphs(Rml::FontFaceHandle face_handle, Rml::StringView string, const Rml::TextShapingContext& context){
    FaceHandle* handle = toHandle(face_handle);
    if (!handle || !currentManager || !RmlRenderer::forRenderManager(currentManager))
        return false;
    // a bitmap face has no distance glyphs: magnified nearest, never re-rasterised smooth (S51 R15)
    if (string.size() == 0 || !renderText || handle->bitmap)
        return true;
    collectDistanceGlyphs(false);
    std::vector<uint32_t> codepoints;
    ShapeOptions options = toShapeOptions(context);
    decodeText(string, context, codepoints, options);
    std::vector<ShapedGlyph> shaped;
    shape(*handle, codepoints, options, shaped);
    bool ready = true;
    for (const ShapedGlyph& glyph : shaped){
        const DistanceEntry& entry = requestDistanceGlyph(*handle, glyph.face, glyph.glyphIndex);
        if (!entry.ready){
            ready = false;
        }else if (entry.page >= 0 && entry.page < (int)distancePages.size()){
            // asked for now: kept through this frame and the next one
            distancePages[(size_t)entry.page]->lastUse = SystemRender::getFrameCount();
        }
    }
    return ready;
}

bool RmlFontEngine::SupportsGpuTextShadow(Rml::FontFaceHandle face_handle, float blur_radius){
    const FaceHandle* handle = toHandle(face_handle);
    if (!handle || handle->bitmap || !gpuEffects() || !currentManager || !RmlRenderer::forRenderManager(currentManager))
        return false;
    // 3 sigma of the blur (sigma = radius / 2) within the distance range
    return 1.5f * std::max(blur_radius, 0.0f) <= RmlFontMsdf::reachEm * (float)handle->size;
}

void RmlFontEngine::ReportTextEffectFallback(Rml::RenderManager& render_manager){
    if (FrameCounters* counters = countersOf(&render_manager))
        counters->textEffectFallbacks++;
}

void RmlFontEngine::ReportTextRegeneration(Rml::RenderManager& render_manager){
    if (FrameCounters* counters = countersOf(&render_manager))
        counters->textRegenerations++;
}

RmlFontEngine::FrameCounters* RmlFontEngine::countersOf(const Rml::RenderManager* renderManager){
    if (!renderManager)
        return nullptr;
    return &managerCounters[renderManager];
}

void RmlFontEngine::beginContextFrame(const Rml::RenderManager& renderManager){
    currentManager = &renderManager;
    managerCounters[&renderManager] = FrameCounters();
    collectDistanceGlyphs(false);
    retryDeferredGlyphs();
}

void RmlFontEngine::deferGlyph(const FaceHandle& handle){
    deferredHandles.insert(const_cast<FaceHandle*>(&handle));
    const uint64_t frame = SystemRender::getFrameCount();
    if (frame != deferredFrame)
        deferredStreak = (deferredStreak > 0 && frame == deferredFrame + 1) ? deferredStreak + 1 : 1;
    deferredFrame = frame;
    // the next frame makes the text again (RenderManager is not const to the context that owns it)
    if (currentManager)
        const_cast<Rml::RenderManager*>(currentManager)->RequestRender();
}

void RmlFontEngine::retryDeferredGlyphs(){
    if (deferredHandles.empty() || SystemRender::getFrameCount() <= deferredFrame)
        return;
    for (FaceHandle* handle : deferredHandles)
        bump(handle);
    deferredHandles.clear();
}

RmlFontEngine::FrameCounters RmlFontEngine::takeContextFrame(const Rml::RenderManager& renderManager){
    FrameCounters counters;
    auto it = managerCounters.find(&renderManager);
    if (it != managerCounters.end()){
        counters = it->second;
        it->second = FrameCounters();
    }
    return counters;
}

//-------------------------------------------------------------------------------------------
// settings
//-------------------------------------------------------------------------------------------

void RmlFontEngine::setAtlasBudget(size_t baseMiB, size_t distanceMiB){
    baseBudgetMiB = baseMiB;
    distanceBudgetMiB = distanceMiB;
}

void RmlFontEngine::setAtlasBudgetMiB(size_t totalMiB){
    if (totalMiB == 0){
        setAtlasBudget(0, 0);
        return;
    }
    const size_t distance = std::max<size_t>(totalMiB / 3, 4);
    setAtlasBudget(std::max<size_t>(totalMiB > distance ? totalMiB - distance : 1, 1), distance);
}

size_t RmlFontEngine::getBaseBudgetBytes(){
    const size_t mib = baseBudgetMiB;
    return (mib ? mib : defaultBaseBudgetMiB()) * MiB;
}

size_t RmlFontEngine::getDistanceBudgetBytes(){
    const size_t mib = distanceBudgetMiB;
    return (mib ? mib : defaultBaseBudgetMiB() / 2) * MiB;
}

uint64_t RmlFontEngine::getEvictionCount() const{
    return evictions;
}

void RmlFontEngine::setTransformAwareTextForTesting(bool enabled){
    transformAwareText = enabled;
}

bool RmlFontEngine::isTransformAwareText(){
    return transformAwareText;
}

void RmlFontEngine::setGpuTextEffectsForTesting(bool enabled){
    gpuTextEffects = enabled;
}

bool RmlFontEngine::isGpuTextEffects(){
    return gpuTextEffects;
}

bool RmlFontEngine::hasFamily(const std::string& name){
    return FontRegistry::hasFamily(name) || isBitmapFamily(name);
}

void RmlFontEngine::setRenderTextForTesting(bool render){
    if (renderText == render)
        return;
    renderText = render;
    bumpEpoch();
}

bool RmlFontEngine::getRenderTextForTesting() const{
    return renderText;
}

int RmlFontEngine::GetVersion(Rml::FontFaceHandle face_handle){
    // (a glyph that waited for room: its text is made again in the next frame, whichever context
    // or harness renders it)
    retryDeferredGlyphs();
    FaceHandle* handle = toHandle(face_handle);
    if (!handle)
        return epoch;
    if (handle->epoch != epoch){
        handle->epoch = epoch;
        handle->version++;
    }
    return handle->version;
}

void RmlFontEngine::clearGlyphs(){
    glyphs.clear();
    distanceGlyphs.clear();
    // the jobs in flight finish, their glyphs are not placed
    for (std::unique_ptr<DistanceJob>& job : distanceJobs)
        job->task.wait();
    distanceJobs.clear();
    // destroying a source releases its textures through the render managers
    pages.clear();
    effectPages.clear();
    distancePages.clear();
    warnedOverflow = false;
}

void RmlFontEngine::ReleaseFontResources(){
    // the pages hold their users: they go first
    clearGlyphs();
    handles.clear();
    deferredHandles.clear();
    retiredHandles.clear();
    widths.clear();
    // RmlUi::ReleaseRenderManagers comes through here: a later manager may reuse an address
    managerScales.clear();
    managerCounters.clear();
    warnedBudget.clear();
    currentManager = nullptr;
    bumpEpoch();
}

void RmlFontEngine::retireHandles(){
    for (auto& entry : handles){
        retiredHandles.push_back(std::move(entry.second));
    }
    handles.clear();
    deferredHandles.clear();
    widths.clear();
    bumpEpoch();
}

bool RmlFontEngine::reloadFontFile(const std::string& path){
    if (pixelart::isBitmapFontPath(path) || isBitmapDependency(path))
        return reloadBitmapFontFile(path);
    std::string error;
    std::vector<std::shared_ptr<FontFace>> replaced;
    if (FontRegistry::reloadFile(normalizeHtmlUIPath(path), &error, &replaced) == 0){
        if (!error.empty())
            Log::error("[HTML UI] %s", error.c_str());
        return false;
    }

    // R18: the old faces' glyphs go (their pages keep the space until they are cleared), and the
    // handles whose chain holds one are retired (a new lookup finds the new face) and bumped
    auto old = [&](const FontFace* face){
        return std::any_of(replaced.begin(), replaced.end(), [face](const std::shared_ptr<FontFace>& f){ return f.get() == face; });
    };
    for (auto it = glyphs.begin(); it != glyphs.end();){
        if (old(it->first.face))
            it = glyphs.erase(it);
        else
            ++it;
    }
    for (std::unique_ptr<DistanceJob>& job : distanceJobs){
        if (old(job->key.face))
            job->task.wait();
    }
    distanceJobs.erase(std::remove_if(distanceJobs.begin(), distanceJobs.end(),
                                      [&](const std::unique_ptr<DistanceJob>& job){ return old(job->key.face); }),
                       distanceJobs.end());
    for (auto it = distanceGlyphs.begin(); it != distanceGlyphs.end();){
        if (old(it->first.face))
            it = distanceGlyphs.erase(it);
        else
            ++it;
    }
    for (auto it = handles.begin(); it != handles.end();){
        FaceHandle* handle = it->second.get();
        if (std::any_of(handle->faces.begin(), handle->faces.end(), old)){
            bump(handle);
            retiredHandles.push_back(std::move(it->second));
            it = handles.erase(it);
        }else{
            ++it;
        }
    }
    widths.clear();
    widthsEpoch = -1;
    return true;
}

void RmlFontEngine::setRasterScale(float scale){
    if (!(scale > 0.0f) || std::fabs(scale - rasterScale) < 1e-6f)
        return;

    rasterScale = scale;
    clearGlyphs();
    bumpEpoch();
}

float RmlFontEngine::getRasterScale() const{
    return rasterScale;
}

void RmlFontEngine::setRasterScale(Rml::RenderManager& renderManager, float scale){
    if (!(scale > 0.0f))
        return;

    const float previous = scaleFor(renderManager);
    managerScales[&renderManager] = scale;
    // that context's text was made at the old scale; glyphs of both scales stay cached
    if (std::fabs(scale - previous) >= 1e-6f)
        bumpEpoch();
}

float RmlFontEngine::getRasterScale(const Rml::RenderManager& renderManager) const{
    return scaleFor(renderManager);
}

size_t RmlFontEngine::getPageCount() const{
    size_t count = 0;
    for (const std::unique_ptr<Page>& page : pages)
        count += page->bytes() > 0;
    return count;
}

size_t RmlFontEngine::getEffectPageCount() const{
    size_t count = 0;
    for (const std::unique_ptr<Page>& page : effectPages)
        count += page->bytes() > 0;
    return count;
}

size_t RmlFontEngine::getDistancePageCount() const{
    size_t count = 0;
    for (const std::unique_ptr<Page>& page : distancePages)
        count += page->bytes() > 0;
    return count;
}

size_t RmlFontEngine::getEffectSetCount() const{
    return effectSets.size();
}

std::vector<const FontFace*> RmlFontEngine::getChain(Rml::FontFaceHandle handle) const{
    FaceHandle* face = toHandle(handle);
    return face ? face->faces : std::vector<const FontFace*>();
}
