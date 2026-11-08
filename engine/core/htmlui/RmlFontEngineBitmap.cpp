// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Bitmap faces of the HTML UI (docs/specs/S51-pixel-fonts.md R15, D7): `@font-face` with a
// .bitmapfont or .fnt source registers a family whose text is drawn from the engine's glyph pages
// (S16's budget and LRU) at a whole pixel scale. `font-size` picks k = max(1, round(size /
// native)); each font pixel is d = max(1, round(k * raster scale)) device pixels, nearest
// replicated: monochrome glyphs in the R8 base pages, coloured ones in the RGBA8 effect pages.
// A pixel outline (`font-effect: outline(1px ...)`) and a sharp shadow are drawn as pixels; glow
// and blur are dropped (`check --ui` reports them). No distance glyphs, no re-raster under a
// transform: the bitmap is magnified nearest.

#include "RmlFontEngine.h"

#include "Log.h"
#include "htmlui/HtmlUIDependencies.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/BitmapText.h"
#include "pixelart/FontPixelReport.h"
#include "render/SystemRender.h"
#include "util/FontRegistry.h"
#include "util/StringUtils.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;

namespace {

    struct BitmapFamily {
        std::string path;
        std::shared_ptr<pixelart::BitmapFont> font;
    };

    // registered faces by normalized family (RmlUi's style sheets register them at load)
    std::map<std::string, BitmapFamily>& rmlBitmapFamilies(){
        static std::map<std::string, BitmapFamily> families;
        return families;
    }

    constexpr uint32_t kRmlBitmapFallback = 0xFFFFFFFFu;

    // the effect a bitmap face draws as pixels: an outline (unique texture, no blur) of w font px,
    // a shadow (the base glyphs moved) of whole font px; anything blurred is dropped
    enum class RmlBitmapEffect { NONE, OUTLINE, SHADOW };

    RmlBitmapEffect rmlBitmapEffectOf(const Rml::FontEffect& effect, int& outline, Rml::Vector2i& shadow){
        Rml::FontEffect::DistanceEffect distance;
        const bool described = effect.GetDistanceEffect(distance);
        if (effect.HasUniqueTexture()){
            if (!described || distance.sigma > 0.0f || distance.dilation <= 0.0f)
                return RmlBitmapEffect::NONE;
            outline = std::clamp((int)std::lround(distance.dilation - 0.5f), 1, 2);
            return RmlBitmapEffect::OUTLINE;
        }
        const Rml::FontGlyph metrics = RmlFontEffects::wrapCoverage(nullptr, 1, 1, 0, 0, 1);
        if (!RmlFontEffects::offset(effect, metrics, shadow))
            return RmlBitmapEffect::NONE;
        return RmlBitmapEffect::SHADOW;
    }

}

bool RmlFontEngine::isBitmapFamily(const std::string& family){
    return rmlBitmapFamilies().count(FontRegistry::normalizeFamily(family)) != 0;
}

std::shared_ptr<pixelart::BitmapFont> RmlFontEngine::bitmapFamilyFont(const std::string& family){
    auto it = rmlBitmapFamilies().find(FontRegistry::normalizeFamily(family));
    return it == rmlBitmapFamilies().end() ? nullptr : it->second.font;
}

bool RmlFontEngine::isBitmapDependency(const std::string& path){
    const std::string wanted = normalizeHtmlUIPath(path);
    for (const auto& entry : rmlBitmapFamilies()){
        for (const std::string& dependency : entry.second.font->getDependencies())
            if (normalizeHtmlUIPath(dependency) == wanted) return true;
    }
    return false;
}

bool RmlFontEngine::loadBitmapFace(const std::string& file, const std::string& family){
    const std::string path = normalizeHtmlUIPath(file);
    pixelart::BitmapFontReport report;
    std::shared_ptr<pixelart::BitmapFont> font = pixelart::BitmapFontPool::get(path, &report);
    if (!font){
        Log::error("[HTML UI] bitmap font %s does not load: %s", path.c_str(),
                   report.errors.empty() ? "unreadable" : (report.errors[0].message + " (" + report.errors[0].fix + ")").c_str());
        return false;
    }
    // without a family (RmlUi's own LoadFontFace) the file's name is the family
    std::string name = family;
    if (name.empty()){
        const size_t slash = path.find_last_of('/');
        name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        name = name.substr(0, name.find('.'));
    }
    for (const std::string& dependency : font->getDependencies())
        HtmlUIFileLog::recordFile(dependency);
    BitmapFamily& entry = rmlBitmapFamilies()[FontRegistry::normalizeFamily(name)];
    // a family registered again with another font: the old font's glyphs go
    if (entry.font && entry.font != font){
        purgeBitmapGlyphs(entry.font->getSerial());
        widths.clear();
        widthsEpoch = -1;
    }
    entry = BitmapFamily{path, font};
    retireHandles();
    return true;
}

size_t RmlFontEngine::getBitmapGlyphCount(uint64_t serial) const{
    size_t count = 0;
    for (const auto& entry : glyphs)
        count += entry.first.bitmap == serial;
    return count;
}

void RmlFontEngine::purgeBitmapGlyphs(uint64_t serial){
    for (auto it = glyphs.begin(); it != glyphs.end();)
        it = (it->first.bitmap == serial) ? glyphs.erase(it) : std::next(it);
}

std::unique_ptr<RmlFontEngine::FaceHandle> RmlFontEngine::makeBitmapHandle(const std::shared_ptr<pixelart::BitmapFont>& font, int size){
    std::unique_ptr<FaceHandle> handle(new FaceHandle());
    const pixelart::BitmapFont& f = *font;
    const int k = pixelart::pixelScaleFor((unsigned int)std::max(size, 1), f.getNativeSize());
    handle->bitmap = font;
    handle->pixelScale = k;
    handle->size = size;
    // layout metrics in document px, from the font (line-height: normal is its line height)
    Rml::FontMetrics& metrics = handle->metrics;
    metrics.size = size;
    metrics.ascent = (float)(f.getBaseline() * k);
    metrics.descent = (float)((f.getLineHeight() - f.getBaseline()) * k);
    metrics.line_spacing = (float)(f.getLineHeight() * k);
    metrics.x_height = (float)(f.getXHeight() * k);
    metrics.underline_position = (float)k;
    metrics.underline_thickness = (float)k;
    metrics.has_ellipsis = f.hasGlyph(0x2026);
    return handle;
}

int RmlFontEngine::bitmapStringWidth(const FaceHandle& handle, Rml::StringView string, const Rml::TextShapingContext& context) const{
    std::vector<uint32_t> codepoints;
    decodeText(string, codepoints);
    std::u32string line(codepoints.begin(), codepoints.end());
    const int fontPixels = handle.bitmap->measure(line);
    const float letterSpacing = context.letter_spacing * (float)codepoints.size();
    return std::max(0, (int)std::lround((float)(fontPixels * handle.pixelScale) + letterSpacing));
}

const RmlFontEngine::GlyphEntry& RmlFontEngine::bitmapGlyph(const FaceHandle& handle, uint32_t codepoint, int d, int style){
    const pixelart::BitmapFont& font = *handle.bitmap;
    // a bitmap face's glyphs share the vector glyphs' map, keyed by the font's serial (an address
    // a later font may reuse never finds the old glyphs) and the codepoint
    GlyphKey key{nullptr, codepoint, d, 0, (size_t)style};
    key.bitmap = font.getSerial();
    auto it = glyphs.find(key);
    if (it != glyphs.end())
        return it->second;

    GlyphEntry entry;
    const pixelart::BitmapGlyph& g = (codepoint == kRmlBitmapFallback) ? font.getFallbackGlyph() : *font.glyph(codepoint);
    std::vector<uint8_t> rgba;
    font.copyGlyphRgba(g, rgba);
    int w = g.w, h = g.h, ox = g.offsetX, oy = g.offsetY - font.getBaseline();
    std::vector<uint8_t> mask((size_t)w * (size_t)h, 0);
    for (size_t i = 0; i < mask.size(); i++) mask[i] = rgba[i * 4 + 3];
    const bool outline = style >= 16;
    if (outline && w > 0 && h > 0){
        // the silhouette grown w font px through 4 or 8 neighbours (the cache's rule, R10)
        const int width = style / 16, neighbors = style % 16;
        const int gw = w + 2 * width, gh = h + 2 * width;
        std::vector<uint8_t> grown((size_t)gw * (size_t)gh, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                grown[(size_t)(y + width) * (size_t)gw + (size_t)(x + width)] = mask[(size_t)y * (size_t)w + (size_t)x] ? 255 : 0;
        for (int step = 0; step < width; step++){
            std::vector<uint8_t> next = grown;
            for (int y = 0; y < gh; y++){
                for (int x = 0; x < gw; x++){
                    if (grown[(size_t)y * (size_t)gw + (size_t)x]) continue;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++){
                            if ((dx == 0 && dy == 0) || (neighbors == 4 && dx != 0 && dy != 0)) continue;
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < gw && ny < gh && grown[(size_t)ny * (size_t)gw + (size_t)nx])
                                next[(size_t)y * (size_t)gw + (size_t)x] = 255;
                        }
                }
            }
            grown.swap(next);
        }
        mask.swap(grown);
        w = gw;
        h = gh;
        ox -= width;
        oy -= width;
    }
    if (w > 0 && h > 0){
        // nearest replicated: each font pixel d x d raster pixels
        const bool colored = style == 0 && font.isColored();
        const int rw = w * d, rh = h * d;
        std::vector<unsigned char> pixels((size_t)rw * (size_t)rh * (colored || outline || style == 1 ? 4u : 1u), 0);
        for (int y = 0; y < rh; y++){
            for (int x = 0; x < rw; x++){
                const size_t src = (size_t)(y / d) * (size_t)w + (size_t)(x / d);
                if (colored){
                    // premultiplied RGBA (the effect pages' format)
                    const uint8_t* p = &rgba[src * 4];
                    unsigned char* q = &pixels[((size_t)y * (size_t)rw + (size_t)x) * 4];
                    for (int c = 0; c < 3; c++) q[c] = (unsigned char)((p[c] * p[3] + 127) / 255);
                    q[3] = p[3];
                }else if (outline || style == 1){
                    unsigned char* q = &pixels[((size_t)y * (size_t)rw + (size_t)x) * 4];
                    q[0] = q[1] = q[2] = q[3] = mask[src];
                }else{
                    pixels[(size_t)y * (size_t)rw + (size_t)x] = mask[src];
                }
            }
        }
        const PageKind kind = (colored || outline || style == 1) ? PageKind::PIXEL_EFFECT : PageKind::PIXEL;
        int page = -1, x = 0, y = 0;
        if (place(kind, pixels.data(), rw, rh, page, x, y, font.getPath(), handle.size)){
            entry.page = page;
            entry.kind = kind;
            entry.x = x;
            entry.y = y;
            entry.width = rw;
            entry.height = rh;
        }else if (placeDeferred){
            deferGlyph(handle);
            static const GlyphEntry none;
            return none;
        }
    }
    entry.xoff = ox * d;
    entry.yoff = oy * d;
    // (place may have cleared a page and entries with it: the map is written now)
    return glyphs[key] = entry;
}

int RmlFontEngine::generateBitmapString(Rml::RenderManager& renderManager, FaceHandle& handle, const RmlFontEffects::Set* effects,
                                        Rml::StringView string, Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                                        const Rml::TextShapingContext& context, Rml::TexturedMeshList& meshList){
    const pixelart::BitmapFont& font = *handle.bitmap;
    const int k = handle.pixelScale;
    // device px per document px: the context's raster scale; never the transform-aware scale
    const float scale = scaleFor(renderManager);
    const int d = std::max(1, (int)std::lround((float)k * scale));
    const float unit = (float)d / scale;   // document px per font px

    std::vector<uint32_t> codepoints;
    decodeText(string, codepoints);
    std::vector<uint32_t> keys;
    std::vector<int> pens;
    int pen = 0;
    for (size_t i = 0; i < codepoints.size(); i++){
        if (i > 0) pen += font.getSpacing() + font.kerning(codepoints[i - 1], codepoints[i]);
        pens.push_back(pen);
        keys.push_back(font.hasGlyph(codepoints[i]) ? codepoints[i] : kRmlBitmapFallback);
        pen += font.advanceOf(codepoints[i]);
    }
    const float letterSpacing = context.letter_spacing;
    const int width = std::max(0, (int)std::lround((float)(pen * k) + letterSpacing * (float)codepoints.size()));
    if (!renderText)
        return width;
    // a blurred text-shadow has no pixel form (check --ui reports it): the shadow is not drawn
    if (context.text_effect_shadow && context.shadow_blur > 0.0f)
        return width;

    // the origin on a device pixel; every glyph a whole number of font pixels from it (R8)
    const Rml::Vector2f origin(std::round(position.x * scale) / scale, std::round(position.y * scale) / scale);
    const uint64_t frame = SystemRender::getFrameCount();
    const float uvScale = 1.0f / (float)pageSize;

    struct Layer {
        int style;                  // 0 the glyphs, 1 masks, 16 * w + n an outline
        Rml::Vector2i offset;       // font px
        Rml::ColourbPremultiplied colour;
    };
    std::vector<Layer> back, front;
    if (effects){
        for (const Rml::SharedPtr<const Rml::FontEffect>& effect : effects->effects){
            int outline = 0;
            Rml::Vector2i shadow(0, 0);
            const RmlBitmapEffect kind = rmlBitmapEffectOf(*effect, outline, shadow);
            if (kind == RmlBitmapEffect::NONE)
                continue;
            Layer layer;
            layer.colour = effect->GetColour().ToPremultiplied(std::max(0.0f, std::min(opacity, 1.0f)));
            if (kind == RmlBitmapEffect::OUTLINE){
                layer.style = 16 * outline + 8;
            }else{
                // a shadow of the glyphs' silhouette, moved whole font pixels
                layer.style = font.isColored() ? 1 : 0;
                layer.offset = shadow;
            }
            (effect->GetLayer() == Rml::FontEffect::Layer::Back ? back : front).push_back(layer);
        }
    }
    Layer text;
    text.style = 0;
    text.offset = Rml::Vector2i(0, 0);
    // (a coloured font's art is multiplied by it: white draws it as drawn)
    text.colour = (opacity < 1.0f) ? colour * std::max(0.0f, opacity) : colour;

    // every glyph of every layer in a page first
    for (const std::vector<Layer>* layers : {&back, &front})
        for (const Layer& layer : *layers)
            for (uint32_t key : keys) bitmapGlyph(handle, key, d, layer.style);
    for (uint32_t key : keys) bitmapGlyph(handle, key, d, 0);
    refreshDirtyPages();

    auto emit = [&](const Layer& layer){
        std::map<std::pair<int, int>, size_t> meshOf;   // (kind, page) -> mesh
        for (size_t i = 0; i < keys.size(); i++){
            const GlyphEntry& entry = bitmapGlyph(handle, keys[i], d, layer.style);
            if (entry.page < 0 || entry.width <= 0)
                continue;
            PageList& list = pagesOf(entry.kind);
            if (entry.page >= (int)list.size() || !list[(size_t)entry.page]->source)
                continue;
            Page& page = *list[(size_t)entry.page];
            page.lastUse = frame;
            markUsed(page, &handle);
            const std::pair<int, int> slot((int)entry.kind, entry.page);
            auto it = meshOf.find(slot);
            if (it == meshOf.end()){
                it = meshOf.emplace(slot, meshList.size()).first;
                meshList.emplace_back();
                meshList.back().texture = page.source->GetTexture(renderManager);
            }
            const Rml::Vector2f quad(origin.x + ((float)(pens[i] + layer.offset.x) * unit) + (float)i * letterSpacing + (float)entry.xoff / scale,
                                     origin.y + (float)layer.offset.y * unit + (float)entry.yoff / scale);
            Rml::MeshUtilities::GenerateQuad(meshList[it->second].mesh, quad, Rml::Vector2f((float)entry.width / scale, (float)entry.height / scale),
                                             layer.colour, Rml::Vector2f(entry.x * uvScale, entry.y * uvScale),
                                             Rml::Vector2f((entry.x + entry.width) * uvScale, (entry.y + entry.height) * uvScale));
        }
    };
    for (const Layer& layer : back) emit(layer);
    emit(text);
    for (const Layer& layer : front) emit(layer);
    return width;
}

bool RmlFontEngine::reloadBitmapFontFile(const std::string& path){
    // the pool drops what read the file; the families load again, the old glyphs go
    pixelart::BitmapFontPool::invalidate(normalizeHtmlUIPath(path));
    bool any = false;
    for (auto& [family, entry] : rmlBitmapFamilies()){
        bool uses = normalizeHtmlUIPath(entry.path) == normalizeHtmlUIPath(path);
        for (const std::string& dependency : entry.font->getDependencies())
            uses = uses || normalizeHtmlUIPath(dependency) == normalizeHtmlUIPath(path);
        if (!uses)
            continue;
        std::shared_ptr<pixelart::BitmapFont> font = pixelart::BitmapFontPool::get(entry.path);
        if (!font){
            Log::error("[HTML UI] bitmap font %s does not load after its change: the old one stays", entry.path.c_str());
            continue;
        }
        purgeBitmapGlyphs(entry.font->getSerial());
        entry.font = font;
        any = true;
    }
    if (any){
        retireHandles();
        widths.clear();
        widthsEpoch = -1;
    }
    return any;
}
