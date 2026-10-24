// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipPool.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "io/Data.h"
#include "io/FileData.h"

#include <filesystem>
#include <map>
#include <mutex>

using namespace jasperengine;

namespace{

    struct PoolEntry{
        std::shared_ptr<const AnimationClip> clip;
    };

    // the sub-clips of one model: clip part as written ("Walk", "0") -> animation index, and
    // each index's name (what the builder is given)
    struct ModelAliases{
        std::map<std::string, int> indices;
        std::vector<std::string> names;
    };

    struct PoolState{
        std::mutex mutex;
        std::map<std::string, PoolEntry> entries;
        // never decrease, survive invalidate and clear (players compare them)
        std::map<std::string, uint64_t> generations;
        // bumped by every invalidate of the key: a load that started before it is not cached
        std::map<std::string, uint64_t> epochs;
        uint64_t clearEpoch = 0;
        std::map<std::string, ModelAliases> models;   // by model key
        uint64_t revision = 0;
    };

    // never destroyed: players may release clips during static destruction
    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

    std::string pathKey(const std::string& path){
        if (path.empty()) return std::string();
        return std::filesystem::path(FileData::getSystemPath(path)).lexically_normal().generic_string();
    }

    // the entry key of a sub-clip index: "<model key>#<index>"
    std::string subClipKey(const std::string& modelKey, int index){
        return modelKey + "#" + std::to_string(index);
    }

    // with the mutex held: the entry key of `path` (sub-clip aliases resolved), empty when a
    // sub-clip alias is not known yet
    std::string entryKeyLocked(PoolState& pool, const std::string& path, bool& subClip){
        anim::SubClipReference reference;
        subClip = anim::parseSubClipReference(path, reference);
        if (!subClip) return pathKey(path);
        std::string modelKey = pathKey(reference.model);
        auto model = pool.models.find(modelKey);
        if (model == pool.models.end()) return std::string();
        auto alias = model->second.indices.find(reference.clip);
        if (alias == model->second.indices.end()) return std::string();
        return subClipKey(modelKey, alias->second);
    }

    void bumpLocked(PoolState& pool, const std::string& key){
        pool.epochs[key]++;
        auto generation = pool.generations.find(key);
        if (generation != pool.generations.end()) generation->second++;
    }

    uint64_t epochLocked(PoolState& pool, const std::string& key){
        auto it = pool.epochs.find(key);
        return it == pool.epochs.end() ? 0 : it->second;
    }

    // Caches `clip` under `key` unless the key was invalidated (or the pool cleared) since the
    // load started; a clip cached meanwhile by another load wins. With the mutex held.
    std::shared_ptr<const AnimationClip> insertLocked(PoolState& pool, const std::string& key, uint64_t epoch, uint64_t clearEpoch,
                                                      std::shared_ptr<const AnimationClip> clip){
        auto existing = pool.entries.find(key);
        if (existing != pool.entries.end()) return existing->second.clip;
        if (epochLocked(pool, key) != epoch || pool.clearEpoch != clearEpoch) return clip;
        pool.entries[key].clip = clip;
        pool.generations[key]++;
        pool.revision++;
        return clip;
    }

    void dropModelLocked(PoolState& pool, const std::string& modelKey){
        std::string prefix = modelKey + "#";
        for (auto it = pool.entries.lower_bound(prefix); it != pool.entries.end() && it->first.compare(0, prefix.size(), prefix) == 0;){
            bumpLocked(pool, it->first);
            it = pool.entries.erase(it);
        }
        // in-flight sub-clip loads of this model that have no entry yet
        for (auto it = pool.epochs.lower_bound(prefix); it != pool.epochs.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it){
            it->second++;
        }
        // in-flight alias table fills
        pool.epochs[prefix]++;
        pool.models.erase(modelKey);
        pool.revision++;
    }

    std::shared_ptr<const AnimationClip> loadFile(const std::string& path, std::string* error, std::vector<ClipWarning>* warnings){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            if (error) *error = "cannot read " + path;
            return nullptr;
        }
        std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        anim::ClipParseResult parsed = anim::parseClip(text);
        if (!parsed.ok()){
            if (error) *error = parsed.error.toString();
            return nullptr;
        }
        if (warnings) *warnings = parsed.warnings;
        return AnimationClip::build(*parsed.data, parsed.warnings, nullptr);
    }

}

std::string AnimationClipPool::getKey(const std::string& path){
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(path, reference)){
        return anim::formatSubClipReference(pathKey(reference.model), reference.clip);
    }
    return pathKey(path);
}

std::shared_ptr<const AnimationClip> AnimationClipPool::get(const std::string& path){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool subClip = false;
    std::string key = entryKeyLocked(pool, path, subClip);
    auto it = key.empty() ? pool.entries.end() : pool.entries.find(key);
    return it == pool.entries.end() ? nullptr : it->second.clip;
}

std::shared_ptr<const AnimationClip> AnimationClipPool::getOrLoad(const std::string& path, std::string* error,
                                                                  std::vector<ClipWarning>* warnings){
    PoolState& pool = state();
    if (warnings) warnings->clear();
    if (path.empty()){
        if (error) *error = "no clip path";
        return nullptr;
    }

    anim::SubClipReference reference;
    if (!anim::parseSubClipReference(path, reference)){
        std::string key = pathKey(path);
        uint64_t epoch = 0, clearEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            auto it = pool.entries.find(key);
            if (it != pool.entries.end()){
                if (warnings) *warnings = it->second.clip->getWarnings();
                return it->second.clip;
            }
            epoch = epochLocked(pool, key);
            clearEpoch = pool.clearEpoch;
        }
        // read and build without the lock (lock order: never held while loading)
        std::shared_ptr<const AnimationClip> clip = loadFile(path, error, warnings);
        if (!clip) return nullptr;
        std::lock_guard<std::mutex> lock(pool.mutex);
        std::shared_ptr<const AnimationClip> cached = insertLocked(pool, key, epoch, clearEpoch, clip);
        if (cached != clip && warnings) *warnings = cached->getWarnings();
        return cached;
    }

    // a model sub-clip: the model's alias table first (filled once per model), then the entry
    // of the animation index (the builders are T1c's ModelSubClips)
    std::string modelKey = pathKey(reference.model);
    std::string aliasEpochKey = modelKey + "#";
    ModelAliases aliases;
    bool known = false;
    uint64_t epoch = 0, clearEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto model = pool.models.find(modelKey);
        if (model != pool.models.end()){
            aliases = model->second;
            known = true;
        }
        epoch = epochLocked(pool, aliasEpochKey);
        clearEpoch = pool.clearEpoch;
    }
    if (!known){
        std::vector<anim::ModelClipInfo> infos;
        if (!anim::listModelFileClips(reference.model, infos, error)) return nullptr;
        for (const anim::ModelClipInfo& info : infos){
            if (info.index < 0) continue;
            if ((size_t)info.index >= aliases.names.size()) aliases.names.resize(info.index + 1);
            aliases.names[info.index] = info.name;
            aliases.indices[info.name] = info.index;
        }
        // a number is an index only when no animation has that name
        for (const anim::ModelClipInfo& info : infos){
            if (info.index >= 0) aliases.indices.emplace(std::to_string(info.index), info.index);
        }
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (epochLocked(pool, aliasEpochKey) == epoch && pool.clearEpoch == clearEpoch){
            pool.models.emplace(modelKey, aliases);
        }
    }
    auto alias = aliases.indices.find(reference.clip);
    if (alias == aliases.indices.end()){
        if (error) *error = "no animation \"" + reference.clip + "\" in " + reference.model;
        return nullptr;
    }
    int index = alias->second;
    std::string key = subClipKey(modelKey, index);
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.entries.find(key);
        if (it != pool.entries.end()){
            if (warnings) *warnings = it->second.clip->getWarnings();
            return it->second.clip;
        }
        epoch = epochLocked(pool, key);
        clearEpoch = pool.clearEpoch;
    }
    ClipData data;
    std::vector<ClipWarning> buildWarnings;
    std::string name = (size_t)index < aliases.names.size() ? aliases.names[index] : std::to_string(index);
    if (!anim::buildModelSubClip(anim::formatSubClipReference(reference.model, name), data, buildWarnings, error)){
        return nullptr;
    }
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data, buildWarnings, nullptr);
    if (warnings) *warnings = clip->getWarnings();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::shared_ptr<const AnimationClip> cached = insertLocked(pool, key, epoch, clearEpoch, clip);
    if (cached != clip && warnings) *warnings = cached->getWarnings();
    return cached;
}

uint64_t AnimationClipPool::getGeneration(const std::string& path){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool subClip = false;
    std::string key = entryKeyLocked(pool, path, subClip);
    if (key.empty()) return 0;
    auto it = pool.generations.find(key);
    return it == pool.generations.end() ? 0 : it->second;
}

uint64_t AnimationClipPool::revision(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.revision;
}

void AnimationClipPool::invalidate(const std::string& path){
    // a model file: every sub-clip of it (spec 9.R13)
    if (anim::isModelFilePath(path)){
        invalidateModel(path);
        return;
    }
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool subClip = false;
    std::string key = entryKeyLocked(pool, path, subClip);
    if (key.empty()) return;
    bumpLocked(pool, key);
    pool.entries.erase(key);
    pool.revision++;
}

void AnimationClipPool::invalidateModel(const std::string& modelPath){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    dropModelLocked(pool, pathKey(modelPath));
}

void AnimationClipPool::clear(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    for (const auto& entry : pool.entries){
        auto generation = pool.generations.find(entry.first);
        if (generation != pool.generations.end()) generation->second++;
    }
    pool.entries.clear();
    pool.models.clear();
    pool.epochs.clear();
    pool.clearEpoch++;
    pool.revision++;
}

void AnimationClipPool::clearUnused(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool changed = false;
    for (auto it = pool.entries.begin(); it != pool.entries.end();){
        if (it->second.clip.use_count() <= 1){
            it = pool.entries.erase(it);
            changed = true;
        }else{
            ++it;
        }
    }
    // alias tables of models with no clip left (the model may change before its next use)
    for (auto it = pool.models.begin(); it != pool.models.end();){
        std::string prefix = it->first + "#";
        auto entry = pool.entries.lower_bound(prefix);
        bool used = entry != pool.entries.end() && entry->first.compare(0, prefix.size(), prefix) == 0;
        if (!used){
            it = pool.models.erase(it);
            changed = true;
        }else{
            ++it;
        }
    }
    if (changed) pool.revision++;
}

void AnimationClipPool::releaseUnused(const std::string& path){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool subClip = false;
    std::string key = entryKeyLocked(pool, path, subClip);
    auto it = key.empty() ? pool.entries.end() : pool.entries.find(key);
    if (it != pool.entries.end() && it->second.clip.use_count() <= 1){
        pool.entries.erase(it);
        pool.revision++;
    }
}

size_t AnimationClipPool::size(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.entries.size();
}
