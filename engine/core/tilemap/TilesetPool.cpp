// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilesetPool.h"

#include "Log.h"
#include "asset/AssetMap.h"
#include "asset/ImportedAssets.h"
#include "io/Data.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

using namespace jasperengine;

namespace {

    using TilesetSheetGenerations = std::vector<std::pair<std::string, uint64_t>>;

    struct TilesetPoolEntry{
        std::shared_ptr<const TilesetRuntime> runtime;
        TilesetSheetGenerations sheets;   // the sheets it was resolved from, at the generation used
    };

    struct TilesetPoolState{
        std::mutex mutex;
        std::map<std::string, TilesetPoolEntry> entries;
        std::map<std::string, uint64_t> epochs;      // bumped by invalidate: in-flight loads are not cached
        std::map<std::string, uint64_t> failed;      // key -> the epoch whose load failed
        std::map<std::string, std::string> errors;
        uint64_t generationCounter = 0;
        uint64_t clearEpoch = 0;
        uint64_t revision = 0;
    };

    // never destroyed: layers may release their runtime during static destruction
    TilesetPoolState& tilesetPoolState(){
        static TilesetPoolState* pool = new TilesetPoolState();
        return *pool;
    }

    uint16_t tilesetPageIndex(TilesetRuntime& runtime, const std::string& texture, int width, int height){
        for (size_t i = 0; i < runtime.pages.size(); i++){
            if (runtime.pages[i] == texture)
                return (uint16_t)i;
        }
        runtime.pages.push_back(texture);
        runtime.pageWidths.push_back(width);
        runtime.pageHeights.push_back(height);
        return (uint16_t)(runtime.pages.size() - 1);
    }

    void tilesetSetUvs(TileRenderInfo& info, double x, double y, double w, double h, int pageWidth, int pageHeight){
        info.u0 = (float)(x / pageWidth);
        info.v0 = (float)(y / pageHeight);
        info.u1 = (float)((x + w) / pageWidth);
        info.v1 = (float)((y + h) / pageHeight);
    }

    struct TilesetResolvedSource{
        bool ok = false;
        const TilesetSource* source = nullptr;
        int tileWidth = 0;
        int tileHeight = 0;
        uint16_t page = 0;
        int pageWidth = 0;
        int pageHeight = 0;
        std::shared_ptr<const SpriteSheetData> sheet;
        std::vector<int> sheetPages;
    };

    // A source's asset by its GUID first (it survives the file's move or rename), else its path
    // hint. In the editor AssetMap asks the asset database; exports write the resolved paths and no
    // GUIDs into the .tileset files they ship, so a game never queries the map for them.
    std::string tilesetSourcePath(const AssetRef& ref){
        std::string found;
        if (!ref.guid.isNull() && AssetMap::findPath(ref.guid, found) && !found.empty())
            return found;
        return ref.path;
    }

    // Runs without the pool's lock: sheet loads read files and call registered loaders.
    std::shared_ptr<TilesetRuntime> tilesetResolve(const std::string& path, std::shared_ptr<const TilesetData> data, TilesetSheetGenerations& sheets){
        auto runtime = std::make_shared<TilesetRuntime>();
        runtime->path = path;
        runtime->data = data;

        std::map<int, TilesetResolvedSource> resolved;
        for (const TilesetSource& source : data->sources){
            TilesetResolvedSource entry;
            entry.source = &source;
            entry.tileWidth = source.tileWidth > 0 ? source.tileWidth : data->tileWidth;
            entry.tileHeight = source.tileHeight > 0 ? source.tileHeight : data->tileHeight;
            const std::string asset = tilesetSourcePath(source.asset);
            switch (source.kind){
                case TilesetSourceKind::Texture:{
                    int width = 0, height = 0;
                    if (asset.empty() || !SpriteSheetSources::textureSize(asset, width, height) || width <= 0 || height <= 0){
                        runtime->warnings.push_back("source " + std::to_string(source.id) + ": cannot read the texture " + asset);
                        break;
                    }
                    entry.page = tilesetPageIndex(*runtime, asset, width, height);
                    entry.pageWidth = width;
                    entry.pageHeight = height;
                    entry.ok = true;
                    break;
                }
                case TilesetSourceKind::Sheet:{
                    entry.sheet = SpriteSheetPool::get(asset);
                    sheets.emplace_back(asset, SpriteSheetPool::generation(asset));
                    if (!entry.sheet){
                        runtime->warnings.push_back("source " + std::to_string(source.id) + ": cannot load the sheet " + asset);
                        break;
                    }
                    for (const SpritePage& page : entry.sheet->pages)
                        entry.sheetPages.push_back(tilesetPageIndex(*runtime, page.texture, page.width, page.height));
                    entry.ok = true;
                    break;
                }
                case TilesetSourceKind::Aseprite:{
                    const std::string reference = ImportedAssets::reference(ImportedAssets::normalize(asset), "tileset#" + std::to_string(source.asepriteTileset));
                    std::string strip;
                    int width = 0, height = 0;
                    if (!ImportedAssets::find(reference, strip) || !SpriteSheetSources::textureSize(strip, width, height) || width <= 0 || height <= 0){
                        runtime->warnings.push_back("source " + std::to_string(source.id) + ": no imported tileset strip for " + reference);
                        break;
                    }
                    entry.page = tilesetPageIndex(*runtime, strip, width, height);
                    entry.pageWidth = width;
                    entry.pageHeight = height;
                    entry.ok = true;
                    break;
                }
            }
            resolved[source.id] = std::move(entry);
        }

        int32_t tileCount = data->nextId;
        for (const TileDef& tile : data->tiles)
            tileCount = std::max(tileCount, tile.id + 1);
        runtime->tiles.resize((size_t)tileCount);

        std::set<int> outsideWarned;
        for (const TileDef& tile : data->tiles){
            if (tile.id < 0)
                continue;
            TileRenderInfo& info = runtime->tiles[(size_t)tile.id];
            info.offsetX = tile.offsetX;
            info.offsetY = tile.offsetY;
            if (!tile.animation.empty()){
                TileAnimationRuntime animation;
                animation.randomStart = tile.randomStart;
                for (const TileAnimationFrame& frame : tile.animation){
                    animation.totalMs += std::max(frame.ms, 1);
                    animation.frames.push_back(frame.tile);
                    animation.endsMs.push_back(animation.totalMs);
                }
                info.animation = (int)runtime->animations.size();
                runtime->animations.push_back(std::move(animation));
            }
            auto found = resolved.find(tile.source);
            if (found == resolved.end() || !found->second.ok)
                continue;
            const TilesetResolvedSource& source = found->second;
            switch (source.source->kind){
                case TilesetSourceKind::Texture:
                case TilesetSourceKind::Aseprite:{
                    const bool strip = source.source->kind == TilesetSourceKind::Aseprite;
                    // the strip's first row is Aseprite's empty tile 0, so our tile i is row i + 1
                    const int x = strip ? 0 : source.source->margin + tile.atX * (source.tileWidth + source.source->spacing);
                    const int y = strip ? (tile.atX + 1) * source.tileHeight : source.source->margin + tile.atY * (source.tileHeight + source.source->spacing);
                    if (x < 0 || y < 0 || x + source.tileWidth > source.pageWidth || y + source.tileHeight > source.pageHeight){
                        if (outsideWarned.insert(source.source->id).second)
                            runtime->warnings.push_back("source " + std::to_string(source.source->id) + ": tile " + std::to_string(tile.id) + " lies outside its image");
                        break;
                    }
                    info.page = source.page;
                    tilesetSetUvs(info, x, y, source.tileWidth, source.tileHeight, source.pageWidth, source.pageHeight);
                    info.x1 = (float)source.tileWidth;
                    info.y1 = (float)source.tileHeight;
                    info.width = source.tileWidth;
                    info.height = source.tileHeight;
                    info.valid = true;
                    break;
                }
                case TilesetSourceKind::Sheet:{
                    const int frameIndex = source.sheet->findFrame(tile.frame);
                    if (frameIndex < 0){
                        runtime->warnings.push_back("tile " + std::to_string(tile.id) + ": no frame \"" + tile.frame + "\" in " + source.source->asset.path);
                        break;
                    }
                    const SpriteFrame& frame = source.sheet->frames[(size_t)frameIndex];
                    const SpritePage& page = source.sheet->pages[frame.page];
                    if (page.width <= 0 || page.height <= 0)
                        break;
                    info.page = (uint16_t)source.sheetPages[frame.page];
                    tilesetSetUvs(info, frame.rect.getX(), frame.rect.getY(), frame.rect.getWidth(), frame.rect.getHeight(), page.width, page.height);
                    // the frame's offset counts from the top-left, the quad from the bottom-left
                    info.x0 = frame.offset.x;
                    info.x1 = frame.offset.x + frame.rect.getWidth();
                    info.y1 = frame.sourceSize.y - frame.offset.y;
                    info.y0 = info.y1 - frame.rect.getHeight();
                    info.width = (int)frame.sourceSize.x;
                    info.height = (int)frame.sourceSize.y;
                    info.valid = true;
                    break;
                }
            }
        }
        return runtime;
    }

    std::shared_ptr<const TilesetData> tilesetLoadFile(const std::string& path, std::vector<std::string>& warnings, std::string& error){
        Data file;
        if (file.open(path.c_str()) != FileErrors::FILEDATA_OK){
            error = "cannot read the file";
            return nullptr;
        }
        const std::string_view text(reinterpret_cast<const char*>(file.getMemPtr()), file.length());
        auto data = std::make_shared<TilesetData>();
        std::vector<TilesetProblem> problems;
        const bool parsed = tileset::parse(text, *data, problems);
        for (const TilesetProblem& problem : problems)
            warnings.push_back(problem.field + ": " + problem.message);
        if (!parsed){
            error = problems.empty() ? std::string("not a tileset") : problems.back().field + ": " + problems.back().message;
            return nullptr;
        }
        return data;
    }

    bool tilesetSheetsCurrent(const TilesetSheetGenerations& sheets){
        for (const auto& [sheet, generation] : sheets){
            if (SpriteSheetPool::generation(sheet) != generation)
                return false;
        }
        return true;
    }

    // publishes a resolved runtime and stamps it, under the pool's lock
    std::shared_ptr<const TilesetRuntime> tilesetPublish(TilesetPoolState& pool, const std::string& key, std::shared_ptr<TilesetRuntime> runtime, TilesetSheetGenerations sheets, bool cache){
        runtime->generation = ++pool.generationCounter;
        std::shared_ptr<const TilesetRuntime> shared = std::move(runtime);
        if (cache){
            pool.entries[key] = TilesetPoolEntry{shared, std::move(sheets)};
            pool.failed.erase(key);
            pool.errors.erase(key);
            pool.revision++;
        }
        return shared;
    }

}

int32_t TilesetRuntime::animatedTile(int32_t id, int64_t ms) const{
    const TileRenderInfo* info = id >= 0 && (size_t)id < tiles.size() ? &tiles[(size_t)id] : nullptr;
    if (!info || info->animation < 0)
        return id;
    const TileAnimationRuntime& animation = animations[(size_t)info->animation];
    if (animation.totalMs <= 0 || animation.frames.empty())
        return id;
    const int64_t total = animation.totalMs;
    const int32_t at = (int32_t)(((ms % total) + total) % total);
    const auto it = std::upper_bound(animation.endsMs.begin(), animation.endsMs.end(), at);
    return it == animation.endsMs.end() ? animation.frames.back() : animation.frames[(size_t)(it - animation.endsMs.begin())];
}

std::shared_ptr<const TilesetRuntime> TilesetPool::find(const std::string& path){
    const std::string key = SpriteSheetPool::getKey(path);
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.entries.find(key);
    return it == pool.entries.end() ? nullptr : it->second.runtime;
}

std::shared_ptr<const TilesetRuntime> TilesetPool::get(const std::string& path){
    if (path.empty())
        return nullptr;
    const std::string key = SpriteSheetPool::getKey(path);
    TilesetPoolState& pool = tilesetPoolState();
    uint64_t epoch = 0, clearEpoch = 0;
    std::shared_ptr<const TilesetData> reuse;
    std::shared_ptr<const TilesetRuntime> stale;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.entries.find(key);
        if (it != pool.entries.end()){
            if (tilesetSheetsCurrent(it->second.sheets))
                return it->second.runtime;
            // a sheet repacked: its frames moved, the document did not
            reuse = it->second.runtime->data;
            stale = it->second.runtime;
        }
        epoch = pool.epochs[key];
        clearEpoch = pool.clearEpoch;
        if (!reuse){
            auto failed = pool.failed.find(key);
            if (failed != pool.failed.end() && failed->second == epoch)
                return nullptr;
        }
    }

    std::vector<std::string> warnings;
    std::string error;
    std::shared_ptr<const TilesetData> data = reuse ? reuse : tilesetLoadFile(path, warnings, error);
    std::shared_ptr<TilesetRuntime> runtime;
    TilesetSheetGenerations sheets;
    if (data){
        runtime = tilesetResolve(path, data, sheets);
        runtime->warnings.insert(runtime->warnings.begin(), warnings.begin(), warnings.end());
    }

    std::lock_guard<std::mutex> lock(pool.mutex);
    const bool current = pool.epochs[key] == epoch && pool.clearEpoch == clearEpoch;
    if (!runtime){
        if (current){
            if (!pool.failed.count(key))
                Log::warn("Tileset %s: %s", path.c_str(), error.c_str());
            pool.failed[key] = epoch;
            pool.errors[key] = error;
        }
        return nullptr;
    }
    // Two threads can re-resolve a stale entry at once: the first one published wins, so a
    // repack bumps the layers' generation once
    if (current && stale){
        auto it = pool.entries.find(key);
        if (it != pool.entries.end() && it->second.runtime != stale)
            return it->second.runtime;
    }
    return tilesetPublish(pool, key, std::move(runtime), std::move(sheets), current);
}

std::shared_ptr<const TilesetRuntime> TilesetPool::resolve(const std::string& path, std::shared_ptr<const TilesetData> data){
    if (!data)
        return nullptr;
    TilesetSheetGenerations sheets;
    std::shared_ptr<TilesetRuntime> runtime = tilesetResolve(path, std::move(data), sheets);
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return tilesetPublish(pool, std::string(), std::move(runtime), TilesetSheetGenerations(), false);
}

void TilesetPool::set(const std::string& path, std::shared_ptr<const TilesetData> data){
    if (path.empty() || !data)
        return;
    const std::string key = SpriteSheetPool::getKey(path);
    TilesetSheetGenerations sheets;
    std::shared_ptr<TilesetRuntime> runtime = tilesetResolve(path, std::move(data), sheets);
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    // a load already running reads the file this write replaced
    pool.epochs[key]++;
    tilesetPublish(pool, key, std::move(runtime), std::move(sheets), true);
}

void TilesetPool::invalidate(const std::string& path){
    const std::string key = SpriteSheetPool::getKey(path);
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.erase(key);
    pool.failed.erase(key);
    pool.errors.erase(key);
    pool.epochs[key]++;
    pool.revision++;
}

void TilesetPool::asepriteSourceChanged(const std::string& asepritePath){
    const std::string changed = ImportedAssets::normalize(asepritePath);
    TilesetPoolState& pool = tilesetPoolState();
    std::vector<std::pair<std::string, std::shared_ptr<const TilesetData>>> affected;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        for (const auto& [key, entry] : pool.entries){
            const std::shared_ptr<const TilesetData>& data = entry.runtime->data;
            if (!data)
                continue;
            for (const TilesetSource& source : data->sources){
                if (source.kind == TilesetSourceKind::Aseprite && ImportedAssets::normalize(source.asset.path) == changed){
                    affected.emplace_back(entry.runtime->path, data);
                    break;
                }
            }
        }
    }
    // outside the lock, as set() resolves: the strip's new file is on disk before this runs
    for (const auto& [path, data] : affected)
        set(path, data);
}

void TilesetPool::clear(){
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.clear();
    pool.failed.clear();
    pool.errors.clear();
    pool.clearEpoch++;
    pool.revision++;
}

uint64_t TilesetPool::revision(){
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.revision;
}

std::string TilesetPool::lastError(const std::string& path){
    const std::string key = SpriteSheetPool::getKey(path);
    TilesetPoolState& pool = tilesetPoolState();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.errors.find(key);
    return it == pool.errors.end() ? std::string() : it->second;
}
