// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapText.h"

#include "pixelart/BitmapFont.h"
#include "buffer/Buffer.h"
#include "math/Vector4.h"
#include "texture/TextureData.h"
#include "util/StringUtils.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::pixelart {

    BitmapText::BitmapText(std::shared_ptr<BitmapFont> font, std::string chainKey, uint32_t generation)
        : font(std::move(font)), chainKey(std::move(chainKey)), generation(generation){
    }

    std::vector<BitmapLine> BitmapText::layoutLines(const BitmapFont& font, const std::vector<uint32_t>& cp, bool wrap, int widthPx){
        // one pass over each line: its width is kept as it is measured (at a break, the width
        // before the break's character)
        std::vector<BitmapLine> lines;
        const int spacing = font.getSpacing();
        size_t start = 0;
        while (true){
            size_t end = start;
            while (end < cp.size() && cp[end] != 10) end++;
            size_t lineStart = start;
            do {
                size_t breakAt = end, lastSpace = end;
                int width = 0, widthAtBreak = 0, widthBeforeSpace = 0;
                bool broke = false;
                for (size_t i = lineStart; i < end; i++){
                    const int before = width;
                    if (cp[i] == 32){
                        lastSpace = i;
                        widthBeforeSpace = before;
                    }
                    if (i > lineStart) width += spacing + font.kerning(cp[i - 1], cp[i]);
                    width += font.advanceOf(cp[i]);
                    if (wrap && width > widthPx && i > lineStart){
                        const bool atSpace = lastSpace > lineStart && lastSpace < i;
                        breakAt = atSpace ? lastSpace : i;
                        widthAtBreak = atSpace ? widthBeforeSpace : before;
                        broke = true;
                        break;
                    }
                }
                lines.push_back({lineStart, breakAt - lineStart, broke ? widthAtBreak : width});
                // a break on a space swallows it, as a newline would
                lineStart = (breakAt < end && cp[breakAt] == 32) ? breakAt + 1 : breakAt;
            } while (wrap && lineStart < end);
            if (end >= cp.size())
                break;
            start = end + 1;
        }
        return lines;
    }

    void BitmapText::measure(const BitmapFont& font, const std::string& text, int& width, int& height){
        bool invalid = false;
        const std::vector<uint32_t> cp = StringUtils::decodeUtf8ToCodepoints(text, invalid);
        const std::vector<BitmapLine> lines = layoutLines(font, cp, false, 0);
        width = 0;
        for (const BitmapLine& line : lines){
            int pen = 0, right = line.width;
            for (size_t i = line.first; i < line.first + line.count; i++){
                if (i > line.first) pen += font.getSpacing() + font.kerning(cp[i - 1], cp[i]);
                const BitmapGlyph* g = font.glyph(cp[i]);
                if (g->w > 0) right = std::max(right, pen + g->offsetX + (int)g->w);
                pen += g->advance;
            }
            width = std::max(width, right);
        }
        height = (int)lines.size() * font.getLineHeight();
    }

    TextureData* BitmapText::getTexture() const{
        if (cachePage >= 0)
            return BitmapTextCache::get().getTexture(cachePage);
        return font ? font->getPageTexture((size_t)page) : nullptr;
    }

    std::string BitmapText::getTextureId() const{
        if (cachePage >= 0)
            return "bitmaptext:" + std::to_string(cachePage) + ":" + std::to_string(cacheGeneration) + ":" + std::to_string(cacheVersion);
        return "bitmapfont:" + std::to_string(font ? font->getSerial() : 0) + ":" + std::to_string(page);
    }

    bool BitmapText::isColoredTexture() const{
        return cachePage >= 0 || (font && font->isColored());
    }

    float BitmapText::getAscent(float unit) const{
        return font ? (float)font->getBaseline() * unit : 0.0f;
    }

    bool BitmapText::isCacheStale() const{
        return cachePage >= 0 && BitmapTextCache::get().getGeneration(cachePage) != cacheGeneration;
    }

    void BitmapText::createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                                std::vector<CharExtent>& charExtents, unsigned int& width, unsigned int& height, const BitmapTextOptions& options,
                                std::vector<TextGlyphInfo>* glyphInfo){
        bool invalid = false;
        const std::vector<uint32_t> cp = StringUtils::decodeUtf8ToCodepoints(text, invalid);
        if (invalid)
            Log::warn("Invalid character");
        const BitmapFont& f = *font;
        const float unit = options.unit > 0.0f ? options.unit : 1.0f;
        const bool wrap = options.fixedWidth && options.multiline;
        const int widthPx = (int)std::floor((float)width / unit + 1e-4f);
        const std::vector<BitmapLine> lines = layoutLines(f, cp, wrap, widthPx);

        // the cache when a style or several pages need one texture
        const int previousPage = cachePage;
        const bool sameRequest = previousPage >= 0 && options.style == lastStyle && cp == acquiredText;
        lastStyle = options.style;
        cachePage = -1;
        backQuads = 0;
        page = 0;
        const bool styled = options.style.hasBack();
        if ((styled || f.getPages().size() > 1) && sameRequest && BitmapTextCache::get().getGeneration(previousPage) == cacheGeneration){
            // the same text and style on a page that still holds its glyphs (a colour change, a
            // move): no glyph list, no acquire
            BitmapTextCache& cache = BitmapTextCache::get();
            cachePage = previousPage;
            cache.touch(cachePage);
            cacheVersion = cache.getVersion(cachePage);
        }else if (styled || f.getPages().size() > 1){
            acquiredText = cp;
            std::vector<uint32_t> list;
            list.reserve(cp.size());
            for (uint32_t c : cp){
                if (c == 10) continue;
                list.push_back(f.hasGlyph(c) ? c : kCacheFallbackGlyph);
            }
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            BitmapTextCache& cache = BitmapTextCache::get();
            cachePage = cache.acquire(f, options.style, list);
            if (cachePage < 0)
                Log::error("Bitmap text: a glyph of %s is larger than a cache page", f.getPath().c_str());
            cacheGeneration = cache.getGeneration(cachePage);
            cacheVersion = cache.getVersion(cachePage);
        }
        const bool fromCache = cachePage >= 0;
        const bool colors = options.vertexColor || fromCache;

        Attribute* atrVertice = buffer->getAttribute(AttributeType::POSITION);
        Attribute* atrTexcoord = buffer->getAttribute(AttributeType::TEXCOORD1);
        Attribute* atrColor = colors ? buffer->getAttribute(AttributeType::COLOR) : nullptr;
        if (glyphInfo)
            glyphInfo->clear();

        // one quad per codepoint shown (a space too: its quad is empty), in logical order
        struct Placed {
            size_t index;
            uint32_t line;
            int pen;
            uint32_t key;               // the codepoint, or the fallback glyph's key
            const BitmapGlyph* glyph;
        };
        std::vector<Placed> placed;
        placed.reserve(cp.size());
        charPositions.clear();
        charExtents.clear();
        std::vector<Vector2> positions(cp.size(), Vector2(0, 0));
        std::vector<CharExtent> extents(cp.size());
        std::vector<bool> shown(cp.size(), false);
        int inkRight = 0;
        for (size_t l = 0; l < lines.size(); l++){
            const BitmapLine& line = lines[l];
            const int lineTop = (int)l * f.getLineHeight();
            int pen = 0;
            for (size_t i = line.first; i < line.first + line.count; i++){
                if (i > line.first) pen += f.getSpacing() + f.kerning(cp[i - 1], cp[i]);
                const bool has = f.hasGlyph(cp[i]);
                const BitmapGlyph* g = f.glyph(cp[i]);
                placed.push_back({i, (uint32_t)l, pen, has ? cp[i] : kCacheFallbackGlyph, g});
                CharExtent& extent = extents[i];
                extent.left = (float)pen * unit;
                extent.right = (float)(pen + g->advance) * unit;
                extent.y = (float)lineTop * unit;
                if (g->w > 0) inkRight = std::max(inkRight, pen + g->offsetX + (int)g->w);
                pen += g->advance;
                positions[i] = Vector2((float)pen * unit, (float)lineTop * unit);
                shown[i] = true;
            }
            // a swallowed break space sits at its line's end
            const size_t after = line.first + line.count;
            if (after < cp.size() && cp[after] == 32 && !shown[after]){
                positions[after] = Vector2((float)pen * unit, (float)lineTop * unit);
                extents[after].left = extents[after].right = (float)pen * unit;
                extents[after].y = (float)lineTop * unit;
                shown[after] = true;
            }
        }
        for (size_t i = 0; i < cp.size(); i++){
            if (cp[i] == 10)
                continue;
            charPositions.push_back(positions[i]);
            charExtents.push_back(extents[i]);
        }

        const float pageW = fromCache ? (float)BitmapTextCache::get().getPageWidth(cachePage)
                                      : (f.getPages().empty() ? 1.0f : (float)f.getPages()[0].width);
        const float pageH = fromCache ? (float)BitmapTextCache::get().getPageHeight(cachePage)
                                      : (f.getPages().empty() ? 1.0f : (float)f.getPages()[0].height);
        int vertex = 0;
        // R20: the buffer is sized once for every quad and each vertex written in place (a
        // Buffer::add per value was most of a rebuild); a buffer that already holds vertices
        // takes the adds
        const size_t quadCount = placed.size() * ((fromCache && styled) ? 2u : 1u);
        const unsigned int stride = buffer->getStride();
        const bool direct = quadCount > 0 && atrVertice && atrTexcoord && atrVertice->getCount() == 0 && stride > 0;
        unsigned char* bytes = nullptr;
        if (direct){
            buffer->increase((size_t)stride * quadCount * 4u);
            bytes = buffer->getData();
        }
        indices.reserve(indices.size() + quadCount * 6u);
        if (glyphInfo) glyphInfo->reserve(quadCount);
        auto write = [&](const Attribute* attribute, int at, const float* values, size_t count){
            std::memcpy(bytes + (size_t)at * stride + attribute->getOffset(), values, count * sizeof(float));
        };
        auto addQuad = [&](int x0, int y0, int w, int h, int u0, int v0, const Vector4& color, const Placed& p, bool back){
            float qy0 = (float)y0 * unit, qy1 = (float)(y0 + h) * unit;
            float t0 = (float)v0 / pageH, t1 = (float)(v0 + h) / pageH;
            if (options.invert){
                const float y = qy0;
                qy0 = -qy1;
                qy1 = -y;
                std::swap(t0, t1);
            }
            const float qx0 = (float)x0 * unit, qx1 = (float)(x0 + w) * unit;
            const float s0 = (float)u0 / pageW, s1 = (float)(u0 + w) / pageW;
            if (direct){
                const float corners[4][5] = {{qx0, qy0, 0.0f, s0, t0}, {qx1, qy0, 0.0f, s1, t0}, {qx1, qy1, 0.0f, s1, t1}, {qx0, qy1, 0.0f, s0, t1}};
                const float rgba[4] = {color.x, color.y, color.z, color.w};
                for (int c = 0; c < 4; c++){
                    write(atrVertice, vertex + c, corners[c], 3);
                    write(atrTexcoord, vertex + c, corners[c] + 3, 2);
                    if (atrColor) write(atrColor, vertex + c, rgba, 4);
                }
            }else{
                buffer->addVector3(atrVertice, Vector3(qx0, qy0, 0));
                buffer->addVector3(atrVertice, Vector3(qx1, qy0, 0));
                buffer->addVector3(atrVertice, Vector3(qx1, qy1, 0));
                buffer->addVector3(atrVertice, Vector3(qx0, qy1, 0));
                buffer->addVector2(atrTexcoord, Vector2(s0, t0));
                buffer->addVector2(atrTexcoord, Vector2(s1, t0));
                buffer->addVector2(atrTexcoord, Vector2(s1, t1));
                buffer->addVector2(atrTexcoord, Vector2(s0, t1));
                if (atrColor)
                    for (int c = 0; c < 4; c++) buffer->addVector4(atrColor, color);
            }
            indices.push_back((uint16_t)vertex);
            indices.push_back((uint16_t)(vertex + 1));
            indices.push_back((uint16_t)(vertex + 2));
            indices.push_back((uint16_t)vertex);
            indices.push_back((uint16_t)(vertex + 2));
            indices.push_back((uint16_t)(vertex + 3));
            vertex += 4;
            if (glyphInfo){
                TextGlyphInfo info;
                info.cluster = (uint32_t)p.index;
                info.line = p.line;
                info.back = back;
                glyphInfo->push_back(info);
            }
        };

        const Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);
        const Vector4 face(options.faceColor.x, options.faceColor.y, options.faceColor.z, 1.0f);
        // the Text's glyphs on its cache page, looked up once
        const std::unordered_map<uint32_t, CachedGlyph>* cached = fromCache ? BitmapTextCache::get().glyphsOf(cachePage, f, options.style) : nullptr;
        // each placed glyph's cache entry, found once for both of its quads
        std::vector<const CachedGlyph*> cachedOf(cached ? placed.size() : 0, nullptr);
        for (size_t i = 0; i < cachedOf.size(); i++){
            auto it = cached->find(placed[i].key);
            if (it != cached->end()) cachedOf[i] = &it->second;
        }
        auto cachedGlyph = [&cachedOf, &placed](const Placed& p) -> const CachedGlyph*{
            return cachedOf.empty() ? nullptr : cachedOf[(size_t)(&p - placed.data())];
        };
        if (fromCache && styled){
            for (const Placed& p : placed){
                const BitmapGlyph* g = p.glyph;
                const CachedGlyph* c = cachedGlyph(p);
                const int x0 = p.pen + g->offsetX, y0 = (int)p.line * f.getLineHeight() + g->offsetY - f.getBaseline();
                if (c && c->backW > 0)
                    addQuad(x0 + c->backOffsetX, y0 + c->backOffsetY, c->backW, c->backH, c->backX, c->backY, white, p, true);
                else
                    addQuad(x0, y0, 0, 0, 0, 0, white, p, true);
                backQuads++;
            }
        }
        for (const Placed& p : placed){
            const BitmapGlyph* g = p.glyph;
            const int x0 = p.pen + g->offsetX, y0 = (int)p.line * f.getLineHeight() + g->offsetY - f.getBaseline();
            if (fromCache){
                const CachedGlyph* c = cachedGlyph(p);
                if (c && c->faceW > 0)
                    addQuad(x0, y0, c->faceW, c->faceH, c->faceX, c->faceY, face, p, false);
                else
                    addQuad(x0, y0, 0, 0, 0, 0, face, p, false);
            }else if (g->page == 0){
                addQuad(x0, y0, g->w, g->h, g->x, g->y, face, p, false);
            }else{
                // a multi-page font whose glyphs found no cache page: the Text binds page 0 only,
                // so a glyph of another page draws nothing rather than page 0's texels there
                addQuad(x0, y0, 0, 0, 0, 0, face, p, false);
            }
        }

        if (direct){
            atrVertice->setCount((unsigned int)vertex);
            atrTexcoord->setCount((unsigned int)vertex);
            if (atrColor) atrColor->setCount((unsigned int)vertex);
            buffer->setCount(std::max(buffer->getCount(), (unsigned int)vertex));
        }

        // an empty text still has geometry (STBText's degenerate triangle)
        if (placed.empty()){
            for (int i = 0; i < 3; i++){
                buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));
                buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
                if (atrColor) buffer->addVector4(atrColor, face);
                indices.push_back((uint16_t)i);
            }
        }

        int widest = inkRight;
        for (const BitmapLine& line : lines) widest = std::max(widest, line.width);
        if (!options.fixedWidth)
            width = (unsigned int)std::ceil((float)widest * unit - 1e-4f);
        if (!options.fixedHeight)
            height = (unsigned int)std::ceil((float)((int)lines.size() * f.getLineHeight()) * unit - 1e-4f);
    }

}
