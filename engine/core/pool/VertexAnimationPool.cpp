// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimationPool.h"

#include "animation/VertexAnimation.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "subsystem/MeshSystem.h"

#include <filesystem>
#include <map>
#include <mutex>

using namespace jasperengine;

namespace{

    struct Entry{
        std::shared_ptr<const vat::VertexAnimation> asset;
        std::vector<ClipWarning> warnings;      // the load's (the parser's) warnings
    };

    struct PoolState{
        std::mutex mutex;
        std::map<std::string, Entry> entries;
        // a key's generation from one pool-wide counter that only grows (the AnimationClipPool
        // rule): survives invalidate and clear
        std::map<std::string, uint64_t> generations;
        uint64_t generationCounter = 0;
        // bumped by every invalidate/put of the key: a load that started before is not cached
        std::map<std::string, uint64_t> epochs;
        uint64_t clearEpoch = 0;
    };

    // never destroyed: components may release assets during static destruction
    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

    // the generated "<stem>.vat.glb" stands for its "<stem>.vat"
    std::string vatPathOf(const std::string& path){
        const std::string suffix = vat::VAT_MESH_SUFFIX;
        if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0){
            return path.substr(0, path.size() - 4);     // drop ".glb"
        }
        return path;
    }

    // the model key rule (FileData::getSystemPath, lexically normal, generic separators)
    std::string pathKey(const std::string& path){
        if (path.empty()) return std::string();
        return MeshSystem::getModelFilenameKey(vatPathOf(path));
    }

    uint64_t epochLocked(PoolState& pool, const std::string& key){
        auto it = pool.epochs.find(key);
        return it == pool.epochs.end() ? 0 : it->second;
    }

    void bumpLocked(PoolState& pool, const std::string& key){
        pool.epochs[key]++;
        pool.generations[key] = ++pool.generationCounter;
    }

    bool readFile(const std::string& path, std::vector<uint8_t>& out){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) return false;
        const uint8_t* bytes = data.getMemPtr();
        out.assign(bytes, bytes + data.length());
        return true;
    }

    // the .vat's directory joined with its `mesh` (asset-relative stays asset-relative)
    std::string meshPath(const std::string& vatPath, const std::string& mesh){
        std::filesystem::path dir = std::filesystem::path(vatPath).parent_path();
        return (dir / std::filesystem::path(mesh)).lexically_normal().generic_string();
    }

    std::shared_ptr<const vat::VertexAnimation> loadFiles(const std::string& path, std::string* error,
                                                         std::vector<ClipWarning>& warnings){
        // the parsers report problems as results; this is the last guard of a loader thread
        try{
            std::vector<uint8_t> bytes;
            if (!readFile(path, bytes)){
                if (error) *error = "cannot read " + path;
                return nullptr;
            }
            vat::VatParseResult parsed = vat::parseVat(std::string(bytes.begin(), bytes.end()));
            warnings = parsed.warnings;
            if (!parsed.ok()){
                if (error) *error = parsed.error;
                return nullptr;
            }
            vat::VatData& data = *parsed.data;

            const std::string mesh = meshPath(path, data.mesh);
            std::vector<uint8_t> glb;
            if (!readFile(mesh, glb)){
                if (error) *error = "cannot read " + mesh + " (the generated mesh of " + path + "): rebake (Inspector > Rebake, "
                                    "or bake_vertex_animation {rebake})";
                return nullptr;
            }
            vat::VatGlbContents contents;
            std::string glbError;
            if (!vat::readVatGlb(glb, contents, &glbError)){
                if (error) *error = mesh + ": " + glbError;
                return nullptr;
            }
            if (contents.layoutHash != data.layout.layoutHash){
                if (error) *error = "generated field layout.layout_hash changed: rebake (Inspector > Rebake, or "
                                    "bake_vertex_animation {rebake})";
                return nullptr;
            }
            std::string createError;
            std::shared_ptr<const vat::VertexAnimation> asset =
                vat::VertexAnimation::create(std::move(data), std::move(contents.payload), std::move(contents.sockets), &createError);
            if (!asset && error) *error = path + ": " + createError;
            return asset;
        }catch (const std::exception& e){
            if (error) *error = std::string("cannot load ") + path + ": " + e.what();
        }catch (...){
            if (error) *error = "cannot load " + path;
        }
        return nullptr;
    }

}

std::string VertexAnimationPool::getKey(const std::string& path){
    return pathKey(path);
}

std::shared_ptr<const vat::VertexAnimation> VertexAnimationPool::get(const std::string& path){
    std::string key = pathKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.entries.find(key);
    return it == pool.entries.end() ? nullptr : it->second.asset;
}

std::shared_ptr<const vat::VertexAnimation> VertexAnimationPool::getOrLoad(const std::string& path, std::string* error,
                                                                         std::vector<ClipWarning>* warnings){
    if (warnings) warnings->clear();
    if (path.empty()){
        if (error) *error = "no vertex animation path";
        return nullptr;
    }
    std::string key = pathKey(path);
    PoolState& pool = state();
    uint64_t epoch = 0;
    uint64_t clearEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.entries.find(key);
        if (it != pool.entries.end()){
            if (warnings) *warnings = it->second.warnings;
            return it->second.asset;
        }
        epoch = epochLocked(pool, key);
        clearEpoch = pool.clearEpoch;
    }

    // read and build without the lock
    std::vector<ClipWarning> loadWarnings;
    std::shared_ptr<const vat::VertexAnimation> asset = loadFiles(vatPathOf(path), error, loadWarnings);
    if (warnings) *warnings = loadWarnings;
    if (!asset) return nullptr;
    // the key, before anyone else sees the asset (made non-const by create: the pool is its friend)
    const_cast<vat::VertexAnimation&>(*asset).path = key;

    std::lock_guard<std::mutex> lock(pool.mutex);
    auto existing = pool.entries.find(key);
    if (existing != pool.entries.end()){
        // another load (or a put) won: everyone gets that one
        if (warnings) *warnings = existing->second.warnings;
        return existing->second.asset;
    }
    // invalidated or cleared while loading: returned, not cached
    if (epochLocked(pool, key) != epoch || pool.clearEpoch != clearEpoch) return asset;
    pool.entries[key] = Entry{asset, loadWarnings};
    pool.generations[key] = ++pool.generationCounter;
    return asset;
}

void VertexAnimationPool::put(const std::string& path, std::shared_ptr<const vat::VertexAnimation> asset){
    if (!asset){
        invalidate(path);
        return;
    }
    std::string key = pathKey(path);
    if (key.empty()) return;
    // the contract: the asset's getPath() becomes the key (assets are made non-const by create)
    const_cast<vat::VertexAnimation&>(*asset).path = key;
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bumpLocked(pool, key);
    pool.entries[key] = Entry{std::move(asset), {}};
}

uint64_t VertexAnimationPool::getGeneration(const std::string& path){
    std::string key = pathKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.generations.find(key);
    return it == pool.generations.end() ? 0 : it->second;
}

void VertexAnimationPool::invalidate(const std::string& path){
    std::string key = pathKey(path);
    if (key.empty()) return;
    std::shared_ptr<const vat::VertexAnimation> dropped;     // released after the lock
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.entries.find(key);
    if (it != pool.entries.end()){
        dropped = std::move(it->second.asset);
        pool.entries.erase(it);
        bumpLocked(pool, key);
    }else{
        // a load in flight is not cached; the generation moves only for a key that had one
        pool.epochs[key]++;
        auto generation = pool.generations.find(key);
        if (generation != pool.generations.end()) generation->second = ++pool.generationCounter;
    }
}

void VertexAnimationPool::clear(){
    std::map<std::string, Entry> dropped;     // released after the lock (their textures too)
    PoolState& pool = state();
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        for (const auto& entry : pool.entries){
            pool.generations[entry.first] = ++pool.generationCounter;
        }
        dropped.swap(pool.entries);
        pool.epochs.clear();
        pool.clearEpoch++;
    }
}

void VertexAnimationPool::clearUnused(){
    std::vector<std::shared_ptr<const vat::VertexAnimation>> dropped;
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    for (auto it = pool.entries.begin(); it != pool.entries.end();){
        if (it->second.asset.use_count() <= 1){
            dropped.push_back(std::move(it->second.asset));
            it = pool.entries.erase(it);
        }else{
            ++it;
        }
    }
}

size_t VertexAnimationPool::size(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.entries.size();
}
