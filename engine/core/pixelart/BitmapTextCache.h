// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPTEXTCACHE_H
#define PIXELART_BITMAPTEXTCACHE_H

// The styled glyph cache (S51 R12): RGBA8 pages of bitmap glyphs with their pixel outline and
// shadow composed (the back layer) beside their face images, so a styled Text draws two quad
// layers from one texture. Shelf-packed with a 1 px gutter and filled per glyph on demand. A page
// no Text drew from this frame or the last can be cleared for another style (spryte's TextCache
// rule); the Texts on it rebuild at their next update. Main thread.

#include "Export.h"
#include "texture/TextureData.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace jasperengine {
    class GlyphAtlas;
}

namespace jasperengine::pixelart {

    class BitmapFont;

    // R10: whole font pixels; colours are sRGB bytes (the page is sampled as an sRGB texture)
    struct JASPERENGINE_API BitmapTextStyle {
        int outlineWidth = 0;           // 0, 1 or 2
        int outlineNeighbors = 8;       // 4 or 8
        uint32_t outlineColor = 0;      // RGBA bytes, r in the low byte
        bool shadow = false;
        int shadowX = 0, shadowY = 0;   // y down
        uint32_t shadowColor = 0;

        bool hasBack() const { return outlineWidth > 0 || shadow; }
        bool operator==(const BitmapTextStyle& o) const{
            return outlineWidth == o.outlineWidth && outlineNeighbors == o.outlineNeighbors && outlineColor == o.outlineColor &&
                   shadow == o.shadow && shadowX == o.shadowX && shadowY == o.shadowY && shadowColor == o.shadowColor;
        }
        bool operator!=(const BitmapTextStyle& o) const { return !(*this == o); }
    };

    struct CachedGlyph {
        uint16_t backX = 0, backY = 0, backW = 0, backH = 0;
        int16_t backOffsetX = 0, backOffsetY = 0;       // from the face image's top-left, font px
        uint16_t faceX = 0, faceY = 0, faceW = 0, faceH = 0;
    };

    inline constexpr uint32_t kCacheFallbackGlyph = 0xFFFFFFFFu;   // a codepoint the font draws as its fallback

    class JASPERENGINE_API BitmapTextCache {
    public:
        static BitmapTextCache& get();

        // the page holding every glyph of `codepoints` (kCacheFallbackGlyph for the fallback) in
        // `style`, made on demand; -1 only when a glyph is larger than a page can be
        int acquire(const BitmapFont& font, const BitmapTextStyle& style, const std::vector<uint32_t>& codepoints);
        const CachedGlyph* find(int page, const BitmapFont& font, const BitmapTextStyle& style, uint32_t codepoint) const;
        // a Text drew from the page in this frame
        void touch(int page);

        bool isValid(int page) const;
        uint32_t getGeneration(int page) const;         // bumped when the page is recycled
        uint32_t getVersion(int page) const;            // bumped when glyphs are added
        int getPageWidth(int page) const;
        int getPageHeight(int page) const;
        TextureData* getTexture(int page) const;
        std::string getTextureId(int page) const;

        // 0: the platform default (2 MiB on Android, iOS and HTML5, 8 MiB on desktop)
        void setBudgetMiB(int mib);
        void setBudgetBytes(size_t bytes);
        size_t getBudgetBytes() const;
        size_t getUsedBytes() const;
        size_t getPageCount() const { return pages.size(); }
        size_t getLiveStyles() const;

        struct Stats {
            uint64_t hits = 0;          // glyphs found already made
            uint64_t misses = 0;        // glyphs made
            uint64_t recycles = 0;      // pages cleared for another style
            uint64_t overBudget = 0;    // pages made past the budget (every page was in use)
            uint64_t diagnostics = 0;   // "budget too small" lines logged
        };
        const Stats& getStats() const { return stats; }

        // tests: the frame instead of the renderer's frame count (0 = the renderer's)
        void setFrameOverride(uint64_t frame) { frameOverride = frame; }
        void clear();

    private:
        using Key = std::tuple<uint64_t, int, int, uint32_t, bool, int, int, uint32_t>;
        static Key keyOf(const BitmapFont& font, const BitmapTextStyle& style);

        struct Page {
            std::unique_ptr<GlyphAtlas> atlas;
            std::map<Key, std::unordered_map<uint32_t, CachedGlyph>> entries;
            uint64_t lastUse = 0;
            uint32_t generation = 1;
            uint32_t version = 1;
            std::unique_ptr<TextureData> texture;
        };

        std::vector<Page> pages;
        std::map<Key, int> home;
        size_t budget = 0;
        int budgetMiB = 0;
        uint64_t frameOverride = 0;
        std::vector<uint64_t> recycleFrames;
        bool diagnosed = false;
        Stats stats;

        uint64_t frame() const;
        int openPage(int width, int height);
        void recycle(int page);
        // makes the glyphs on the page; false when one does not fit (those made stay)
        bool fill(int page, const Key& key, const BitmapFont& font, const BitmapTextStyle& style, const std::vector<uint32_t>& codepoints);
        size_t pageBytes(const Page& page) const;
    };

}

#endif //PIXELART_BITMAPTEXTCACHE_H
