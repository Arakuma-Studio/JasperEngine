// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapTextCache.h"

#include "pixelart/BitmapFont.h"
#include "render/SystemRender.h"
#include "util/GlyphAtlas.h"
#include "Engine.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::pixelart {

    namespace{
        constexpr int kMinPage = 128;
        constexpr uint64_t kDiagnosticWindow = 120;
        constexpr size_t kDiagnosticRecycles = 4;

        void unpackColor(uint32_t color, uint8_t out[4]){
            for (int i = 0; i < 4; i++) out[i] = (uint8_t)(color >> (8 * i));
        }

        // straight-alpha "over"
        void over(uint8_t* dst, const uint8_t* src){
            const float sa = src[3] / 255.0f, da = dst[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa <= 0.0f){
                std::memset(dst, 0, 4);
                return;
            }
            for (int c = 0; c < 3; c++)
                dst[c] = (uint8_t)std::lround((src[c] * sa + dst[c] * da * (1.0f - sa)) / oa);
            dst[3] = (uint8_t)std::lround(oa * 255.0f);
        }

        // the ink grown by `width` pixels through 4 or 8 neighbours, `grow` texels around
        std::vector<uint8_t> dilate(const std::vector<uint8_t>& mask, int w, int h, int width, int neighbors, int grow){
            const int ow = w + 2 * grow, oh = h + 2 * grow;
            std::vector<uint8_t> current((size_t)ow * (size_t)oh, 0);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    current[(size_t)(y + grow) * (size_t)ow + (size_t)(x + grow)] = mask[(size_t)y * (size_t)w + (size_t)x];
            for (int step = 0; step < width; step++){
                std::vector<uint8_t> next = current;
                for (int y = 0; y < oh; y++){
                    for (int x = 0; x < ow; x++){
                        if (current[(size_t)y * (size_t)ow + (size_t)x])
                            continue;
                        bool on = false;
                        for (int dy = -1; dy <= 1 && !on; dy++){
                            for (int dx = -1; dx <= 1 && !on; dx++){
                                if ((dx == 0 && dy == 0) || (neighbors == 4 && dx != 0 && dy != 0))
                                    continue;
                                const int nx = x + dx, ny = y + dy;
                                on = nx >= 0 && ny >= 0 && nx < ow && ny < oh && current[(size_t)ny * (size_t)ow + (size_t)nx];
                            }
                        }
                        if (on) next[(size_t)y * (size_t)ow + (size_t)x] = 1;
                    }
                }
                current.swap(next);
            }
            return current;
        }
    }

    BitmapTextCache& BitmapTextCache::get(){
        static BitmapTextCache cache;
        return cache;
    }

    BitmapTextCache::Key BitmapTextCache::keyOf(const BitmapFont& font, const BitmapTextStyle& s){
        return Key(font.getSerial(), s.outlineWidth, s.outlineWidth > 0 ? s.outlineNeighbors : 0, s.outlineWidth > 0 ? s.outlineColor : 0u,
                   s.shadow, s.shadow ? s.shadowX : 0, s.shadow ? s.shadowY : 0, s.shadow ? s.shadowColor : 0u);
    }

    uint64_t BitmapTextCache::frame() const{
        return frameOverride ? frameOverride : SystemRender::getFrameCount();
    }

    void BitmapTextCache::setBudgetMiB(int mib){
        budgetMiB = std::max(0, mib);
        budget = 0;
        trimToBudget();
    }

    void BitmapTextCache::setBudgetBytes(size_t bytes){
        budget = bytes;
        trimToBudget();
    }

    void BitmapTextCache::park(int index){
        Page& page = pages[(size_t)index];
        for (const auto& entry : page.entries){
            auto it = home.find(entry.first);
            if (it != home.end() && it->second == index)
                home.erase(it);
        }
        page.entries.clear();
        page.atlas->reset(0, 0);
        page.texture.reset();
        page.generation++;
        page.version++;
    }

    void BitmapTextCache::trimToBudget(){
        // the least recently used pages no Text drew from this frame or the last; a drawn page
        // stays (a later acquire finds the budget full and recycles)
        const uint64_t now = frame();
        while (getUsedBytes() > getBudgetBytes()){
            int idle = -1;
            for (int i = 0; i < (int)pages.size(); i++){
                const Page& page = pages[(size_t)i];
                if (pageBytes(page) > 0 && page.lastUse + 1 < now && (idle < 0 || page.lastUse < pages[(size_t)idle].lastUse))
                    idle = i;
            }
            if (idle < 0)
                break;
            park(idle);
        }
    }

    size_t BitmapTextCache::getBudgetBytes() const{
        if (budget > 0)
            return budget;
        if (budgetMiB > 0)
            return (size_t)budgetMiB * 1024u * 1024u;
        const Platform platform = Engine::getPlatform();
        const bool small = platform == Platform::Android || platform == Platform::iOS || platform == Platform::Web;
        return (size_t)(small ? 2 : 8) * 1024u * 1024u;
    }

    size_t BitmapTextCache::pageBytes(const Page& page) const{
        return page.atlas ? (size_t)page.atlas->getWidth() * page.atlas->getHeight() * 4u : 0u;
    }

    size_t BitmapTextCache::getUsedBytes() const{
        size_t used = 0;
        for (const Page& page : pages) used += pageBytes(page);
        return used;
    }

    size_t BitmapTextCache::getLiveStyles() const{
        return home.size();
    }

    int BitmapTextCache::openPage(int width, int height){
        Page page;
        page.atlas = std::make_unique<GlyphAtlas>((unsigned int)width, (unsigned int)height, 1, 4);
        page.atlas->reserveLeading(1);
        page.lastUse = frame();
        pages.push_back(std::move(page));
        Page& made = pages.back();
        made.texture = std::make_unique<TextureData>(width, height, (unsigned int)(width * height * 4), ColorFormat::RGBA, 4,
                                                     (void*)made.atlas->getPixels().data());
        return (int)pages.size() - 1;
    }

    void BitmapTextCache::recycle(int index){
        Page& page = pages[(size_t)index];
        for (const auto& entry : page.entries){
            auto it = home.find(entry.first);
            if (it != home.end() && it->second == index)
                home.erase(it);
        }
        page.entries.clear();
        page.atlas->reset(page.atlas->getWidth(), page.atlas->getHeight());
        page.atlas->reserveLeading(1);
        page.generation++;
        page.version++;
        stats.recycles++;
        const uint64_t now = frame();
        recycleFrames.push_back(now);
        recycleFrames.erase(std::remove_if(recycleFrames.begin(), recycleFrames.end(),
                                           [now](uint64_t f){ return f + kDiagnosticWindow < now; }), recycleFrames.end());
        if (recycleFrames.size() > kDiagnosticRecycles && !diagnosed){
            diagnosed = true;
            stats.diagnostics++;
            Log::warn("Bitmap text cache budget too small (%zu styles live)", getLiveStyles());
        }
    }

    bool BitmapTextCache::fill(int index, const Key& key, const BitmapFont& font, const BitmapTextStyle& style, const std::vector<uint32_t>& codepoints){
        Page& page = pages[(size_t)index];
        std::unordered_map<uint32_t, CachedGlyph>& glyphs = page.entries[key];
        const int grow = style.outlineWidth;
        uint8_t outline[4], shadow[4];
        unpackColor(style.outlineColor, outline);
        unpackColor(style.shadowColor, shadow);
        bool changed = false;
        std::vector<uint8_t> rgba;
        for (uint32_t codepoint : codepoints){
            if (glyphs.count(codepoint)){
                stats.hits++;
                continue;
            }
            const BitmapGlyph& g = (codepoint == kCacheFallbackGlyph) ? font.getFallbackGlyph() : *font.glyph(codepoint);
            CachedGlyph cached;
            font.copyGlyphRgba(g, rgba);
            const int w = g.w, h = g.h;
            if (w > 0 && h > 0){
                int fx = 0, fy = 0;
                if (!page.atlas->pack(w, h, fx, fy)){
                    if (changed) page.version++;
                    return false;
                }
                page.atlas->blit(rgba.data(), w, h, fx, fy);
                cached.faceX = (uint16_t)fx;
                cached.faceY = (uint16_t)fy;
                cached.faceW = (uint16_t)w;
                cached.faceH = (uint16_t)h;
                changed = true;
                if (style.hasBack()){
                    std::vector<uint8_t> mask((size_t)w * (size_t)h);
                    for (size_t i = 0; i < mask.size(); i++) mask[i] = rgba[i * 4 + 3] ? 1 : 0;
                    // the silhouette: the outlined glyph (grow texels around), the face without an outline
                    const std::vector<uint8_t> silhouette = dilate(mask, w, h, style.outlineWidth, style.outlineNeighbors, grow);
                    const int sw = w + 2 * grow, sh = h + 2 * grow;
                    const int sx = style.shadow ? style.shadowX : 0, sy = style.shadow ? style.shadowY : 0;
                    const int minX = std::min(0, sx), minY = std::min(0, sy);
                    const int bw = sw + std::abs(sx), bh = sh + std::abs(sy);
                    std::vector<uint8_t> back((size_t)bw * (size_t)bh * 4, 0);
                    if (style.shadow){
                        for (int y = 0; y < sh; y++)
                            for (int x = 0; x < sw; x++)
                                if (silhouette[(size_t)y * (size_t)sw + (size_t)x])
                                    std::memcpy(&back[((size_t)(y + sy - minY) * (size_t)bw + (size_t)(x + sx - minX)) * 4], shadow, 4);
                    }
                    if (style.outlineWidth > 0){
                        for (int y = 0; y < sh; y++)
                            for (int x = 0; x < sw; x++)
                                if (silhouette[(size_t)y * (size_t)sw + (size_t)x])
                                    over(&back[((size_t)(y - minY) * (size_t)bw + (size_t)(x - minX)) * 4], outline);
                    }
                    int bx = 0, by = 0;
                    if (!page.atlas->pack(bw, bh, bx, by)){
                        page.version++;
                        return false;
                    }
                    page.atlas->blit(back.data(), bw, bh, bx, by);
                    cached.backX = (uint16_t)bx;
                    cached.backY = (uint16_t)by;
                    cached.backW = (uint16_t)bw;
                    cached.backH = (uint16_t)bh;
                    cached.backOffsetX = (int16_t)(minX - grow);
                    cached.backOffsetY = (int16_t)(minY - grow);
                }
            }
            glyphs[codepoint] = cached;
            stats.misses++;
        }
        if (changed)
            page.version++;
        return true;
    }

    int BitmapTextCache::acquire(const BitmapFont& font, const BitmapTextStyle& style, const std::vector<uint32_t>& codepoints){
        const Key key = keyOf(font, style);
        const uint64_t now = frame();
        auto use = [this, &key, now](int index){
            pages[(size_t)index].lastUse = now;
            home[key] = index;
            return index;
        };

        auto it = home.find(key);
        if (it != home.end() && fill(it->second, key, font, style, codepoints))
            return use(it->second);
        const int current = (it != home.end()) ? it->second : -1;

        // another page with room
        for (int i = 0; i < (int)pages.size(); i++){
            if (i != current && fill(i, key, font, style, codepoints))
                return use(i);
        }

        // the size the request needs: the minimum page, doubled until its glyphs fit
        size_t area = 0;
        const int grow = style.outlineWidth;
        const int extra = 2 * grow + (style.shadow ? std::max(std::abs(style.shadowX), std::abs(style.shadowY)) : 0);
        int largest = 0;
        for (uint32_t c : codepoints){
            const BitmapGlyph& g = (c == kCacheFallbackGlyph) ? font.getFallbackGlyph() : *font.glyph(c);
            area += (size_t)(g.w + 1) * (size_t)(g.h + 1) + (style.hasBack() ? (size_t)(g.w + extra + 1) * (size_t)(g.h + extra + 1) : 0u);
            largest = std::max(largest, std::max((int)g.w, (int)g.h) + extra + 2);
        }
        int size = kMinPage;
        while (size < kMaxAtlasPage && ((size_t)size * (size_t)size * 10 < area * 13 || size < largest))
            size *= 2;
        if (largest > kMaxAtlasPage)
            return -1;

        // a new page within the budget; a size the shelves could not fill tries the next one
        for (int s = size; s <= kMaxAtlasPage; s *= 2){
            if (getUsedBytes() + (size_t)s * (size_t)s * 4u > getBudgetBytes())
                break;
            const int made = openPage(s, s);
            if (fill(made, key, font, style, codepoints))
                return use(made);
            pages.pop_back();
        }

        // the least recently used page no Text drew from this frame or the last
        int idle = -1;
        for (int i = 0; i < (int)pages.size(); i++){
            const Page& page = pages[(size_t)i];
            if (page.lastUse + 1 < now && (int)page.atlas->getWidth() >= size && (idle < 0 || page.lastUse < pages[(size_t)idle].lastUse))
                idle = i;
        }
        if (idle >= 0){
            recycle(idle);
            if (fill(idle, key, font, style, codepoints))
                return use(idle);
        }

        // every page is in use: past the budget rather than a missing glyph
        stats.overBudget++;
        if (!diagnosed){
            diagnosed = true;
            stats.diagnostics++;
            Log::warn("Bitmap text cache budget too small (%zu styles live)", getLiveStyles() + 1);
        }
        for (int s = size; s <= kMaxAtlasPage; s *= 2){
            const int made = openPage(s, s);
            if (fill(made, key, font, style, codepoints))
                return use(made);
            pages.pop_back();
        }
        return -1;
    }

    const CachedGlyph* BitmapTextCache::find(int index, const BitmapFont& font, const BitmapTextStyle& style, uint32_t codepoint) const{
        const std::unordered_map<uint32_t, CachedGlyph>* glyphs = glyphsOf(index, font, style);
        if (!glyphs)
            return nullptr;
        auto glyph = glyphs->find(codepoint);
        return glyph == glyphs->end() ? nullptr : &glyph->second;
    }

    const std::unordered_map<uint32_t, CachedGlyph>* BitmapTextCache::glyphsOf(int index, const BitmapFont& font, const BitmapTextStyle& style) const{
        if (!isValid(index))
            return nullptr;
        const Page& page = pages[(size_t)index];
        auto entry = page.entries.find(keyOf(font, style));
        return entry == page.entries.end() ? nullptr : &entry->second;
    }

    void BitmapTextCache::touch(int index){
        if (isValid(index))
            pages[(size_t)index].lastUse = frame();
    }

    bool BitmapTextCache::isValid(int index) const{
        return index >= 0 && index < (int)pages.size();
    }

    uint32_t BitmapTextCache::getGeneration(int index) const{
        return isValid(index) ? pages[(size_t)index].generation : 0;
    }

    uint32_t BitmapTextCache::getVersion(int index) const{
        return isValid(index) ? pages[(size_t)index].version : 0;
    }

    int BitmapTextCache::getPageWidth(int index) const{
        return isValid(index) ? (int)pages[(size_t)index].atlas->getWidth() : 0;
    }

    int BitmapTextCache::getPageHeight(int index) const{
        return isValid(index) ? (int)pages[(size_t)index].atlas->getHeight() : 0;
    }

    TextureData* BitmapTextCache::getTexture(int index) const{
        return isValid(index) ? pages[(size_t)index].texture.get() : nullptr;
    }

    std::string BitmapTextCache::getTextureId(int index) const{
        if (!isValid(index))
            return std::string();
        const Page& page = pages[(size_t)index];
        return "bitmaptext:" + std::to_string(index) + ":" + std::to_string(page.generation) + ":" + std::to_string(page.version);
    }

    void BitmapTextCache::clear(){
        pages.clear();
        home.clear();
        recycleFrames.clear();
        diagnosed = false;
        stats = Stats();
    }

}
