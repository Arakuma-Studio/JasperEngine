// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSheetPool.h"

#include "Log.h"
#include "asset/ImportedAssets.h"
#include "io/Data.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetSources.h"
#include "subsystem/MeshSystem.h"
#include "texture/CookedTextures.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

using namespace jasperengine;

namespace {

    struct Source {
        std::function<bool(const std::string&)> accepts;
        SpriteSheetPool::Loader loader;
    };

    struct PoolState {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<const SpriteSheetData>> sheets;
        std::map<std::string, uint64_t> generations;
        std::map<std::string, uint64_t> epochs;      // bumped by invalidate: in-flight loads are not cached
        std::map<std::string, uint64_t> failed;      // key -> the epoch whose load failed
        std::vector<Source> sources;
        uint64_t generationCounter = 0;
        uint64_t clearEpoch = 0;
        uint64_t revision = 0;
    };

    // never destroyed: sprites may release sheets during static destruction
    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

    bool endsWithNoCase(const std::string& text, const char* suffix){
        const size_t n = std::char_traits<char>::length(suffix);
        if (text.size() < n) return false;
        for (size_t i = 0; i < n; i++){
            if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != suffix[i]) return false;
        }
        return true;
    }

    bool isTexturePath(const std::string& path){
        for (const char* ext : {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".hdr", ".pic", ".pnm"}){
            if (endsWithNoCase(path, ext)) return true;
        }
        return false;
    }

    std::shared_ptr<const SpriteSheetData> loadFromSources(const std::string& path, std::string& error){
        SpriteSheetSources::ensureRegistered();
        std::vector<Source> sources;
        {
            PoolState& pool = state();
            std::lock_guard<std::mutex> lock(pool.mutex);
            sources = pool.sources;
        }
        try{
            for (const Source& source : sources){
                if (source.accepts && source.accepts(path)) return source.loader(path);
            }
            if (SpriteSheetPool::isAsepritePath(path)) return SpriteSheetPool::loadImported(path, &error);
            if (isTexturePath(path)) return SpriteSheetPool::loadTexture(path, &error);
            error = "not a sprite sheet source (an .aseprite file, a texture or a .spriteatlas)";
        }catch (const std::exception& e){
            error = e.what();
        }
        return nullptr;
    }

}

bool SpriteSheetPool::isAsepritePath(const std::string& path){
    return endsWithNoCase(path, ".aseprite") || endsWithNoCase(path, ".ase");
}

bool SpriteSheetPool::mapInlineRect(const std::string& texture, const Rect& rect, SpriteFrame& out){
    return SpriteSheetSources::mapInlineRect(texture, rect, out);
}

std::string SpriteSheetPool::getKey(const std::string& path){
    if (path.empty()) return std::string();
    return MeshSystem::getModelFilenameKey(path);
}

std::shared_ptr<const SpriteSheetData> SpriteSheetPool::loadImported(const std::string& path, std::string* error){
    const std::string source = ImportedAssets::normalize(path);
    std::string file;
    if (source.empty() || !ImportedAssets::find(ImportedAssets::reference(source, "sheet"), file)){
        if (error) *error = "no imported sheet for " + path + " (not imported yet, or the import failed)";
        return nullptr;
    }
    Data data;
    if (data.open(file.c_str()) != FileErrors::FILEDATA_OK){
        if (error) *error = "cannot read " + file;
        return nullptr;
    }
    const std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    SpriteSheetParseResult parsed = parseSheet(text);
    if (!parsed.ok()){
        if (error) *error = file + ": " + parsed.error;
        return nullptr;
    }
    for (const std::string& w : parsed.warnings){
        Log::warn("%s: %s", file.c_str(), w.c_str());
    }
    SpriteSheetData& sheet = *parsed.sheet;
    // pages through the same index: the editor's cache, or an export's je-imported/ files
    for (size_t i = 0; i < sheet.pages.size(); i++){
        std::string page;
        if (ImportedAssets::find(ImportedAssets::reference(source, "image#" + std::to_string(i)), page)){
            sheet.pages[i].texture = page;
        }
    }
    return std::make_shared<const SpriteSheetData>(std::move(sheet));
}

std::shared_ptr<const SpriteSheetData> SpriteSheetPool::loadTexture(const std::string& path, std::string* error){
    int width = 0, height = 0;
    CookedTextureEntry cooked;
    if (CookedTextures::find(path, cooked) && cooked.width > 0 && cooked.height > 0){
        width = cooked.width;
        height = cooked.height;
    }else{
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            if (error) *error = "cannot read " + path;
            return nullptr;
        }
        int comp = 0;
        if (!stbi_info_from_memory(data.getMemPtr(), static_cast<int>(data.length()), &width, &height, &comp)){
            if (error) *error = path + " is not an image this engine reads";
            return nullptr;
        }
    }
    SpriteSheetData sheet;
    sheet.source = path;
    SpritePage page;
    page.texture = path;
    page.width = width;
    page.height = height;
    sheet.pages.push_back(page);
    SpriteFrame frame;
    frame.name = std::filesystem::path(path).stem().string();
    frame.rect = Rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    frame.sourceSize = Vector2(static_cast<float>(width), static_cast<float>(height));
    frame.pivot = Vector2(static_cast<float>(width / 2), static_cast<float>(height / 2));
    sheet.frames.push_back(frame);
    return std::make_shared<const SpriteSheetData>(std::move(sheet));
}

std::shared_ptr<const SpriteSheetData> SpriteSheetPool::find(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.sheets.find(key);
    return it == pool.sheets.end() ? nullptr : it->second;
}

std::shared_ptr<const SpriteSheetData> SpriteSheetPool::get(const std::string& path){
    if (path.empty()) return nullptr;
    const std::string key = getKey(path);
    PoolState& pool = state();
    uint64_t epoch = 0, clearEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.sheets.find(key);
        if (it != pool.sheets.end()) return it->second;
        epoch = pool.epochs[key];
        clearEpoch = pool.clearEpoch;
        auto failed = pool.failed.find(key);
        if (failed != pool.failed.end() && failed->second == epoch) return nullptr;
    }
    // outside the lock: a load reads files and may call a registered source's loader
    std::string error;
    std::shared_ptr<const SpriteSheetData> sheet = loadFromSources(path, error);
    std::lock_guard<std::mutex> lock(pool.mutex);
    const bool current = pool.epochs[key] == epoch && pool.clearEpoch == clearEpoch;
    if (!sheet){
        if (current && !pool.failed.count(key)) Log::error("Sprite sheet %s: %s", path.c_str(), error.c_str());
        if (current) pool.failed[key] = epoch;
        return nullptr;
    }
    if (current){
        pool.sheets[key] = sheet;
        pool.failed.erase(key);
        pool.generations[key] = ++pool.generationCounter;
        pool.revision++;
    }
    return sheet;
}

uint64_t SpriteSheetPool::generation(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.generations.find(key);
    return it == pool.generations.end() ? 0 : it->second;
}

void SpriteSheetPool::invalidate(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.sheets.erase(key);
    pool.failed.erase(key);
    pool.epochs[key]++;
    auto generation = pool.generations.find(key);
    if (generation != pool.generations.end()) generation->second = ++pool.generationCounter;
    pool.revision++;
}

void SpriteSheetPool::clear(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.sheets.clear();
    pool.failed.clear();
    pool.clearEpoch++;
    for (auto& [key, generation] : pool.generations) generation = ++pool.generationCounter;
    pool.revision++;
}

uint64_t SpriteSheetPool::revision(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.revision;
}

void SpriteSheetPool::registerSource(std::function<bool(const std::string& path)> accepts, Loader loader){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.sources.push_back({std::move(accepts), std::move(loader)});
    pool.revision++;
}
