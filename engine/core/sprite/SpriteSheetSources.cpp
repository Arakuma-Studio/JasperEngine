// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSheetSources.h"

#include "Log.h"
#include "asset/ImportedAssets.h"
#include "io/Data.h"
#include "sprite/SpriteSheetPool.h"
#include "texture/CookedTextures.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

using namespace jasperengine;

namespace {

    bool sourcesEndsWithNoCase(const std::string& text, const char* suffix){
        const size_t n = std::char_traits<char>::length(suffix);
        if (text.size() < n) return false;
        for (size_t i = 0; i < n; i++){
            if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != suffix[i]) return false;
        }
        return true;
    }

    // a preset's pivot in pixels from a w x h frame's top-left; custom: normalized from the bottom-left
    Vector2 settingsPivot(const TextureSheetSettings& settings, float w, float h){
        if (settings.customPivot){
            return Vector2(std::round(settings.pivotNormalized.x * w), std::round((1.0f - settings.pivotNormalized.y) * h));
        }
        const float cx = std::floor(w / 2.0f), cy = std::floor(h / 2.0f);
        switch (settings.pivotPreset){
            case PivotPreset::TOP_CENTER: return Vector2(cx, 0.0f);
            case PivotPreset::BOTTOM_CENTER: return Vector2(cx, h);
            case PivotPreset::LEFT_CENTER: return Vector2(0.0f, cy);
            case PivotPreset::RIGHT_CENTER: return Vector2(w, cy);
            case PivotPreset::TOP_LEFT: return Vector2(0.0f, 0.0f);
            case PivotPreset::BOTTOM_LEFT: return Vector2(0.0f, h);
            case PivotPreset::TOP_RIGHT: return Vector2(w, 0.0f);
            case PivotPreset::BOTTOM_RIGHT: return Vector2(w, h);
            case PivotPreset::CENTER:
            case PivotPreset::FRAME: break;
        }
        return Vector2(cx, cy);
    }

    std::string uniqueName(const std::string& wanted, std::set<std::string>& used, std::vector<std::string>* warnings){
        std::string name = wanted;
        for (int n = 2; used.count(name); n++){
            name = wanted + " " + std::to_string(n);
        }
        if (name != wanted && warnings) warnings->push_back("frame name \"" + wanted + "\" is used twice: the second is \"" + name + "\"");
        used.insert(name);
        return name;
    }

    // the texture's pixels, for a "name@WxH" file loaded without an editor
    std::shared_ptr<const SpriteSheetData> loadCellSizeTexture(const std::string& path){
        int cellW = 0, cellH = 0;
        std::string base;
        if (!SpriteSheetSources::parseCellSize(path, cellW, cellH, &base)) throw std::runtime_error("not a name@WxH texture");
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) throw std::runtime_error("cannot read " + path);
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load_from_memory(data.getMemPtr(), static_cast<int>(data.length()), &w, &h, &comp, 4);
        if (!pixels) throw std::runtime_error(path + " is not an image this engine reads (a cooked texture needs its sheet exported)");
        std::vector<std::string> warnings;
        SpriteSheetData sheet = SpriteSheetSources::buildTextureSheet(path, w, h, SpriteSheetSources::cellSizeSettings(cellW, cellH, base),
            [&](int x, int y, int rw, int rh){
                for (int yy = std::max(0, y); yy < std::min(h, y + rh); yy++){
                    for (int xx = std::max(0, x); xx < std::min(w, x + rw); xx++){
                        if (pixels[(static_cast<size_t>(yy) * w + xx) * 4 + 3] != 0) return true;
                    }
                }
                return false;
            }, &warnings);
        stbi_image_free(pixels);
        for (const std::string& warning : warnings) Log::warn("%s: %s", path.c_str(), warning.c_str());
        return std::make_shared<const SpriteSheetData>(std::move(sheet));
    }

    std::shared_ptr<const SpriteSheetData> loadImportedOrThrow(const std::string& path){
        std::string error;
        std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::loadImported(path, &error);
        if (!sheet) throw std::runtime_error(error);
        return sheet;
    }

    bool hasImportedSheet(const std::string& path){
        std::string file;
        return ImportedAssets::find(ImportedAssets::reference(ImportedAssets::normalize(path), "sheet"), file);
    }

    // mapInlineRect's view of each texture: the pool's sheet while an atlas packs it
    struct InlineEntry {
        uint64_t revision = 0;
        bool packed = false;
        std::shared_ptr<const SpriteSheetData> sheet;
        int width = 0, height = 0;
    };

    struct InlineCache {
        std::mutex mutex;
        std::map<std::string, InlineEntry> entries;
    };

    InlineCache& inlineCache(){
        static InlineCache* cache = new InlineCache();
        return *cache;
    }

}

bool TextureSheetSettings::operator==(const TextureSheetSettings& o) const{
    auto sameGrid = [](const SpriteGridSettings& a, const SpriteGridSettings& b){
        return a.cellWidth == b.cellWidth && a.cellHeight == b.cellHeight && a.offsetX == b.offsetX && a.offsetY == b.offsetY &&
               a.spacingX == b.spacingX && a.spacingY == b.spacingY && a.count == b.count && a.skipEmpty == b.skipEmpty && a.prefix == b.prefix;
    };
    if (mode != o.mode || !sameGrid(grid, o.grid) || frames.size() != o.frames.size() || pivotPreset != o.pivotPreset ||
        customPivot != o.customPivot || pivotNormalized != o.pivotNormalized) return false;
    for (size_t i = 0; i < frames.size(); i++){
        const TextureSheetFrame& a = frames[i];
        const TextureSheetFrame& b = o.frames[i];
        Rect ra = a.rect, rb = b.rect;
        if (a.name != b.name || ra != rb || a.hasPivot != b.hasPivot || a.pivot != b.pivot || a.border != b.border) return false;
    }
    return true;
}

bool SpriteSheetSources::isAtlasPath(const std::string& path){
    return sourcesEndsWithNoCase(path, ".spriteatlas");
}

bool SpriteSheetSources::isTexturePath(const std::string& path){
    for (const char* ext : {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".hdr", ".pic", ".pnm"}){
        if (sourcesEndsWithNoCase(path, ext)) return true;
    }
    return false;
}

bool SpriteSheetSources::parseCellSize(const std::string& path, int& cellWidth, int& cellHeight, std::string* base){
    const std::string stem = std::filesystem::path(path).stem().string();
    const size_t at = stem.rfind('@');
    if (at == std::string::npos || at == 0) return false;
    const std::string size = stem.substr(at + 1);
    const size_t x = size.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= size.size()) return false;
    int w = 0, h = 0;
    const auto pw = std::from_chars(size.data(), size.data() + x, w);
    const auto ph = std::from_chars(size.data() + x + 1, size.data() + size.size(), h);
    if (pw.ec != std::errc() || pw.ptr != size.data() + x || ph.ec != std::errc() || ph.ptr != size.data() + size.size()) return false;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    cellWidth = w;
    cellHeight = h;
    if (base) *base = stem.substr(0, at);
    return true;
}

TextureSheetSettings SpriteSheetSources::cellSizeSettings(int cellWidth, int cellHeight, const std::string& base){
    TextureSheetSettings settings;
    settings.mode = TextureSpriteMode::GRID;
    settings.grid.cellWidth = cellWidth;
    settings.grid.cellHeight = cellHeight;
    settings.grid.skipEmpty = true;
    settings.grid.prefix = base + "_";
    return settings;
}

std::string SpriteSheetSources::frameTextureKey(const Rect& rect){
    char text[96];
    std::snprintf(text, sizeof(text), "rect:%d,%d,%d,%d", static_cast<int>(rect.getX()), static_cast<int>(rect.getY()),
                  static_cast<int>(rect.getWidth()), static_cast<int>(rect.getHeight()));
    return text;
}

bool SpriteSheetSources::frameTextureRect(const SpriteFrame& frame, Rect& out){
    if (frame.key.rfind("rect:", 0) != 0) return false;
    int v[4] = {0, 0, 0, 0};
    const char* p = frame.key.data() + 5;
    const char* end = frame.key.data() + frame.key.size();
    for (int i = 0; i < 4; i++){
        const auto parsed = std::from_chars(p, end, v[i]);
        if (parsed.ec != std::errc()) return false;
        p = parsed.ptr;
        if (i < 3){
            if (p >= end || *p != ',') return false;
            p++;
        }
    }
    if (p != end) return false;
    out = Rect(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3]));
    return true;
}

SpriteSheetData SpriteSheetSources::buildTextureSheet(const std::string& texture, int width, int height, const TextureSheetSettings& settings,
                                                      const OpaqueProbe& opaque, std::vector<std::string>* warnings){
    SpriteSheetData sheet;
    sheet.source = texture;
    sheet.pages.push_back({texture, width, height});
    std::set<std::string> used;
    const std::string stem = std::filesystem::path(texture).stem().string();
    auto addFrame = [&](const std::string& name, const Rect& rect, bool hasPivot, const Vector2& pivot, const Vector4& border){
        SpriteFrame frame;
        frame.name = uniqueName(name, used, warnings);
        frame.key = frameTextureKey(rect);
        frame.rect = rect;
        frame.sourceSize = Vector2(rect.getWidth(), rect.getHeight());
        frame.pivot = hasPivot ? pivot : settingsPivot(settings, rect.getWidth(), rect.getHeight());
        frame.border = border;
        sheet.frames.push_back(frame);
    };
    if (width <= 0 || height <= 0) return sheet;

    if (settings.mode == TextureSpriteMode::SINGLE){
        addFrame(stem, Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), false, Vector2(), Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        return sheet;
    }
    if (settings.mode == TextureSpriteMode::MULTIPLE){
        for (size_t i = 0; i < settings.frames.size(); i++){
            const TextureSheetFrame& f = settings.frames[i];
            Rect r = f.rect;
            const int x = static_cast<int>(r.getX()), y = static_cast<int>(r.getY());
            const int w = static_cast<int>(r.getWidth()), h = static_cast<int>(r.getHeight());
            const std::string name = f.name.empty() ? stem + "_" + std::to_string(i) : f.name;
            if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > width || y + h > height){
                if (warnings){
                    warnings->push_back("frame \"" + name + "\" [" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(w) + ", " +
                                        std::to_string(h) + "] is not inside the " + std::to_string(width) + "x" + std::to_string(height) +
                                        " image (left out)");
                }
                continue;
            }
            addFrame(name, Rect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)), f.hasPivot, f.pivot, f.border);
        }
        return sheet;
    }

    const SpriteGridSettings& g = settings.grid;
    if (g.cellWidth <= 0 || g.cellHeight <= 0){
        if (warnings) warnings->push_back("the grid's cell size is not set (no frames)");
        return sheet;
    }
    const int stepX = g.cellWidth + std::max(0, g.spacingX);
    const int stepY = g.cellHeight + std::max(0, g.spacingY);
    const int ox = std::max(0, g.offsetX), oy = std::max(0, g.offsetY);
    int columns = 0, rows = 0;
    for (int x = ox; x + g.cellWidth <= width; x += stepX) columns++;
    for (int y = oy; y + g.cellHeight <= height; y += stepY) rows++;
    if (columns == 0 || rows == 0){
        if (warnings){
            warnings->push_back("a " + std::to_string(g.cellWidth) + "x" + std::to_string(g.cellHeight) + " cell does not fit the " +
                                std::to_string(width) + "x" + std::to_string(height) + " image (no frames)");
        }
        return sheet;
    }
    const int leftX = width - ox - (columns * g.cellWidth + (columns - 1) * std::max(0, g.spacingX));
    const int leftY = height - oy - (rows * g.cellHeight + (rows - 1) * std::max(0, g.spacingY));
    if ((leftX > 0 || leftY > 0) && warnings){
        warnings->push_back("the " + std::to_string(g.cellWidth) + "x" + std::to_string(g.cellHeight) + " grid leaves " + std::to_string(leftX) +
                            " pixel(s) on the right and " + std::to_string(leftY) + " at the bottom of the " + std::to_string(width) + "x" +
                            std::to_string(height) + " image");
    }
    const std::string prefix = g.prefix.empty() ? stem + "_" : g.prefix;
    int kept = 0;
    for (int row = 0; row < rows; row++){
        for (int column = 0; column < columns; column++){
            if (g.count > 0 && kept >= g.count) return sheet;
            const int x = ox + column * stepX, y = oy + row * stepY;
            if (g.skipEmpty && opaque && !opaque(x, y, g.cellWidth, g.cellHeight)) continue;
            addFrame(prefix + std::to_string(kept),
                     Rect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(g.cellWidth), static_cast<float>(g.cellHeight)),
                     false, Vector2(), Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            kept++;
        }
    }
    return sheet;
}

bool SpriteSheetSources::textureSize(const std::string& texture, int& width, int& height){
    CookedTextureEntry cooked;
    if (CookedTextures::find(texture, cooked) && cooked.width > 0 && cooked.height > 0){
        width = cooked.width;
        height = cooked.height;
        return true;
    }
    Data data;
    if (data.open(texture.c_str()) != FileErrors::FILEDATA_OK) return false;
    int comp = 0;
    return stbi_info_from_memory(data.getMemPtr(), static_cast<int>(data.length()), &width, &height, &comp) != 0;
}

namespace {
    InlineEntry inlineLookup(const std::string& texture){
        const uint64_t revision = SpriteSheetPool::revision();
        const std::string key = SpriteSheetPool::getKey(texture);
        InlineEntry entry;
        {
            InlineCache& cache = inlineCache();
            std::lock_guard<std::mutex> lock(cache.mutex);
            auto it = cache.entries.find(key);
            if (it != cache.entries.end() && it->second.revision == revision) return it->second;
        }
        entry.revision = revision;
        std::string atlas;
        entry.packed = ImportedAssets::find(ImportedAssets::reference(ImportedAssets::normalize(texture), "atlas"), atlas);
        if (entry.packed){
            entry.sheet = SpriteSheetPool::get(texture);
            entry.packed = entry.sheet && !entry.sheet->frames.empty();
            // the remap names its frames' places in the texture; a single frame is the whole image
            if (entry.packed && !SpriteSheetSources::textureSize(texture, entry.width, entry.height)){
                entry.width = static_cast<int>(entry.sheet->frames[0].sourceSize.x);
                entry.height = static_cast<int>(entry.sheet->frames[0].sourceSize.y);
            }
        }
        InlineCache& cache = inlineCache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        // (the pool may have moved on while this loaded: the next call looks again)
        cache.entries[key] = entry;
        return entry;
    }
}

bool SpriteSheetSources::isPacked(const std::string& texture){
    return !texture.empty() && isTexturePath(texture) && inlineLookup(texture).packed;
}

bool SpriteSheetSources::mapInlineRect(const std::string& texture, const Rect& rect, SpriteFrame& out, int* sourceWidth, int* sourceHeight){
    if (texture.empty() || !isTexturePath(texture)) return false;
    const InlineEntry entry = inlineLookup(texture);
    if (!entry.packed) return false;
    if (sourceWidth) *sourceWidth = entry.width;
    if (sourceHeight) *sourceHeight = entry.height;

    const float rx = rect.getX(), ry = rect.getY(), rw = std::fabs(rect.getWidth()), rh = std::fabs(rect.getHeight());
    if (rw <= 0.0f || rh <= 0.0f) return false;
    for (const SpriteFrame& frame : entry.sheet->frames){
        Rect place;
        if (!frameTextureRect(frame, place)){
            if (entry.sheet->frames.size() != 1) continue;
            place = Rect(0.0f, 0.0f, frame.sourceSize.x, frame.sourceSize.y);
        }
        if (rx < place.getX() || ry < place.getY() || rx + rw > place.getX() + place.getWidth() || ry + rh > place.getY() + place.getHeight()) continue;
        if (frame.page >= entry.sheet->pages.size()) return false;
        // the rect inside the frame's source, then clipped to the packed (trimmed) pixels
        const float lx = rx - place.getX(), ly = ry - place.getY();
        const float tx0 = frame.offset.x, ty0 = frame.offset.y;
        const float tx1 = tx0 + frame.rect.getWidth(), ty1 = ty0 + frame.rect.getHeight();
        const float vx0 = std::max(lx, tx0), vy0 = std::max(ly, ty0);
        const float vx1 = std::min(lx + rw, tx1), vy1 = std::min(ly + rh, ty1);
        out = SpriteFrame();
        out.name = frame.name;
        out.page = frame.page;
        out.sourceSize = Vector2(rw, rh);
        out.pivot = Vector2(rw / 2.0f, rh / 2.0f);
        if (vx1 <= vx0 || vy1 <= vy0){
            out.rect = Rect(frame.rect.getX(), frame.rect.getY(), 0.0f, 0.0f);   // only transparent pixels: nothing drawn
        }else{
            out.rect = Rect(frame.rect.getX() + (vx0 - tx0), frame.rect.getY() + (vy0 - ty0), vx1 - vx0, vy1 - vy0);
            out.offset = Vector2(vx0 - lx, vy0 - ly);
        }
        return true;
    }
    return false;
}

void SpriteSheetSources::ensureRegistered(){
    static std::once_flag once;
    std::call_once(once, [](){
        SpriteSheetPool::registerSource([](const std::string& path){ return isAtlasPath(path); }, &loadImportedOrThrow);
        // a texture an atlas in use packs, or with a Sprite Mode: what the editor or the export wrote
        SpriteSheetPool::registerSource([](const std::string& path){ return isTexturePath(path) && hasImportedSheet(path); }, &loadImportedOrThrow);
        SpriteSheetPool::registerSource([](const std::string& path){
            int w = 0, h = 0;
            return isTexturePath(path) && parseCellSize(path, w, h);
        }, &loadCellSizeTexture);
    });
}
