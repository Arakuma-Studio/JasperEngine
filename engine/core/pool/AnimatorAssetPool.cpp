// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorAssetPool.h"

#include "animation/AnimatorAssetIO.h"
#include "animation/CompiledController.h"
#include "io/Data.h"
#include "subsystem/MeshSystem.h"

#include <map>
#include <mutex>

using namespace jasperengine;

namespace{

    struct Entry{
        std::shared_ptr<const void> value;
        uint64_t generation = 0;
        std::vector<ClipWarning> warnings;
    };

    struct PoolState{
        std::mutex mutex;
        std::map<std::string, Entry> entries;
        std::map<std::string, uint64_t> generations;    // survives invalidate
        uint64_t counter = 0;
        uint64_t revision = 0;
    };

    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

    bool readText(const std::string& path, std::string& text, std::string* error){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            if (error) *error = "cannot read " + path;
            return false;
        }
        text.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    template<class T, class Load>
    std::shared_ptr<const T> getOrLoad(const std::string& path, std::string* error, uint64_t* generation,
                                       std::vector<ClipWarning>* warnings, Load load){
        const std::string key = AnimatorAssetPool::getKey(path);
        PoolState& pool = state();
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            auto it = pool.entries.find(key);
            if (it != pool.entries.end()){
                if (generation) *generation = it->second.generation;
                if (warnings) *warnings = it->second.warnings;
                return std::static_pointer_cast<const T>(it->second.value);
            }
        }
        std::vector<ClipWarning> loadWarnings;
        std::shared_ptr<const T> value;
        try{
            std::string text;
            if (readText(path, text, error)) value = load(text, loadWarnings, error);
        }catch (const std::exception& e){
            if (error) *error = std::string("cannot load ") + path + ": " + e.what();
        }
        if (!value) return nullptr;
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.entries.find(key);
        if (it != pool.entries.end()){
            // another thread loaded it meanwhile: everyone shares that one
            if (generation) *generation = it->second.generation;
            if (warnings) *warnings = it->second.warnings;
            return std::static_pointer_cast<const T>(it->second.value);
        }
        Entry entry;
        entry.value = value;
        entry.generation = ++pool.counter;
        entry.warnings = loadWarnings;
        pool.generations[key] = entry.generation;
        pool.revision++;
        if (generation) *generation = entry.generation;
        if (warnings) *warnings = loadWarnings;
        pool.entries[key] = std::move(entry);
        return value;
    }

}

std::string AnimatorAssetPool::getKey(const std::string& path){
    if (path.empty()) return std::string();
    return MeshSystem::getModelFilenameKey(path);
}

std::shared_ptr<const anim::CompiledController> AnimatorAssetPool::controller(const std::string& path, std::string* error,
                                                                             uint64_t* generation, std::vector<ClipWarning>* warnings){
    return getOrLoad<anim::CompiledController>(path, error, generation, warnings,
        [](const std::string& text, std::vector<ClipWarning>& loadWarnings, std::string* loadError) -> std::shared_ptr<const anim::CompiledController>{
            anim::ControllerParseResult parsed = anim::parseController(text);
            if (!parsed.ok){
                if (loadError) *loadError = parsed.error.toString();
                return nullptr;
            }
            loadWarnings = parsed.warnings;
            return anim::compileController(parsed.data);
        });
}

std::shared_ptr<const anim::AvatarMaskDef> AnimatorAssetPool::mask(const std::string& path, std::string* error,
                                                                  uint64_t* generation, std::vector<ClipWarning>* warnings){
    return getOrLoad<anim::AvatarMaskDef>(path, error, generation, warnings,
        [](const std::string& text, std::vector<ClipWarning>& loadWarnings, std::string* loadError) -> std::shared_ptr<const anim::AvatarMaskDef>{
            anim::MaskParseResult parsed = anim::parseMask(text);
            if (!parsed.ok){
                if (loadError) *loadError = parsed.error.toString();
                return nullptr;
            }
            loadWarnings = parsed.warnings;
            return std::make_shared<const anim::AvatarMaskDef>(std::move(parsed.data));
        });
}

std::shared_ptr<const anim::MontageDef> AnimatorAssetPool::montage(const std::string& path, std::string* error,
                                                                  uint64_t* generation, std::vector<ClipWarning>* warnings){
    return getOrLoad<anim::MontageDef>(path, error, generation, warnings,
        [](const std::string& text, std::vector<ClipWarning>& loadWarnings, std::string* loadError) -> std::shared_ptr<const anim::MontageDef>{
            anim::MontageParseResult parsed = anim::parseMontage(text);
            if (!parsed.ok){
                if (loadError) *loadError = parsed.error.toString();
                return nullptr;
            }
            loadWarnings = parsed.warnings;
            return std::make_shared<const anim::MontageDef>(std::move(parsed.data));
        });
}

uint64_t AnimatorAssetPool::getGeneration(const std::string& path){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.entries.find(getKey(path));
    return it == pool.entries.end() ? 0 : it->second.generation;
}

uint64_t AnimatorAssetPool::revision(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.revision;
}

void AnimatorAssetPool::invalidate(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.erase(key);
    pool.revision++;
}

void AnimatorAssetPool::clear(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.clear();
    pool.revision++;
}

void AnimatorAssetPool::clearUnused(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    for (auto it = pool.entries.begin(); it != pool.entries.end();){
        if (it->second.value.use_count() <= 1) it = pool.entries.erase(it);
        else ++it;
    }
}

size_t AnimatorAssetPool::size(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.entries.size();
}
