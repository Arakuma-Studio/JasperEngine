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
#include <set>

namespace jasperengine::pixelart {

    BitmapText::BitmapText(std::shared_ptr<BitmapFont> font, std::string chainKey, uint32_t generation)
        : font(std::move(font)), chainKey(std::move(chainKey)), generation(generation){
    }

    std::vector<BitmapLine> BitmapText::layoutLines(const BitmapFont& font, const std::vector<uint32_t>& cp, bool wrap, int widthPx){
        std::vector<BitmapLine> lines;
        auto lineWidth = [&font, &cp](size_t first, size_t count){
            int width = 0;
            for (size_t i = first; i < first + count; i++){
                if (i > first) width += font.getSpacing() + font.kerning(cp[i - 1], cp[i]);
                width += font.advanceOf(cp[i]);
            }
            return width;
        };
        size_t start = 0;
        while (true){
            size_t end = start;
            while (end < cp.size() && cp[end] != 10) end++;
            if (wrap && end > start){
                size_t lineStart = start;
                while (lineStart < end){
                    size_t breakAt = end, lastSpace = end;
                    int width = 0;
                    for (size_t i = lineStart; i < end; i++){
                        if (cp[i] == 32) lastSpace = i;
                        if (i > lineStart) width += font.getSpacing() + font.kerning(cp[i - 1], cp[i]);
                        width += font.advanceOf(cp[i]);
                        if (width > widthPx && i > lineStart){
                            breakAt = (lastSpace > lineStart && lastSpace < i) ? lastSpace : i;
                            break;
                        }
                    }
                    lines.push_back({lineStart, breakAt - lineStart, lineWidth(lineStart, breakAt - lineStart)});
                    // a break on a space swallows it, as a newline would
                    lineStart = (breakAt < end && cp[breakAt] == 32) ? breakAt + 1 : breakAt;
                }
            }else{
                lines.push_back({start, end - start, lineWidth(start, end - start)});
            }
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

        lastStyle = options.style;
        // the cache when a style or several pages need one texture
        cachePage = -1;
        backQuads = 0;
        page = 0;
        const bool styled = options.style.hasBack();
        if (styled || f.getPages().size() > 1){
            std::set<uint32_t> needed;
            for (uint32_t c : cp){
                if (c == 10) continue;
                needed.insert(f.hasGlyph(c) ? c : kCacheFallbackGlyph);
            }
            std::vector<uint32_t> list(needed.begin(), needed.end());
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
        BitmapTextCache& cache = BitmapTextCache::get();
        if (fromCache && styled){
            for (const Placed& p : placed){
                const BitmapGlyph* g = p.glyph;
                const CachedGlyph* c = cache.find(cachePage, f, options.style, p.key);
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
                const CachedGlyph* c = cache.find(cachePage, f, options.style, p.key);
                if (c && c->faceW > 0)
                    addQuad(x0, y0, c->faceW, c->faceH, c->faceX, c->faceY, face, p, false);
                else
                    addQuad(x0, y0, 0, 0, 0, 0, face, p, false);
            }else{
                addQuad(x0, y0, g->w, g->h, g->x, g->y, face, p, false);
            }
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
