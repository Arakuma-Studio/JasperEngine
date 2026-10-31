// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsMaterialPool.h"

#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <set>

using namespace jasperengine;

namespace{

    struct PoolState{
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<PhysicsMaterialRuntime>> entries;
        // paths whose failure was logged, until they are invalidated
        std::set<std::string> reported;
    };

    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

}

std::string PhysicsMaterialPool::getKey(const std::string& path){
    if (path.empty()) return std::string();
    return std::filesystem::path(FileData::getSystemPath(path)).lexically_normal().generic_string();
}

std::shared_ptr<PhysicsMaterialRuntime> PhysicsMaterialPool::get(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    auto it = pool.entries.find(key);
    return it == pool.entries.end() ? nullptr : it->second;
}

std::shared_ptr<PhysicsMaterialRuntime> PhysicsMaterialPool::getOrLoad(const std::string& path, std::string* error){
    if (path.empty()){
        if (error) *error = "empty physics material path";
        return nullptr;
    }
    const std::string key = getKey(path);
    PoolState& pool = state();
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto it = pool.entries.find(key);
        if (it != pool.entries.end()) return it->second;
    }

    std::string loadError;
    std::shared_ptr<PhysicsMaterialRuntime> material;
    try{
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            loadError = "cannot read physics material " + path;
        }else{
            std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
            PhysicsMaterialParseResult parsed = physicsmaterial::parse(text);
            if (!parsed.ok){
                loadError = "physics material " + path + ": " + parsed.error;
            }else{
                for (const std::string& warning : parsed.warnings){
                    Log::warn("Physics material %s: %s", path.c_str(), warning.c_str());
                }
                material = std::make_shared<PhysicsMaterialRuntime>(parsed.data, path);
            }
        }
    }catch (const std::exception& e){
        loadError = "cannot load physics material " + path + ": " + e.what();
    }catch (...){
        loadError = "cannot load physics material " + path;
    }

    std::lock_guard<std::mutex> lock(pool.mutex);
    if (!material){
        if (pool.reported.insert(key).second){
            Log::error("%s", loadError.c_str());
        }
        if (error) *error = loadError;
        return nullptr;
    }
    auto inserted = pool.entries.emplace(key, material);
    if (inserted.second){
        PhysicsMaterialRuntime::bumpRevision();
    }
    return inserted.first->second;
}

void PhysicsMaterialPool::invalidate(const std::string& path){
    const std::string key = getKey(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.erase(key);
    pool.reported.erase(key);
    PhysicsMaterialRuntime::bumpRevision();
}

void PhysicsMaterialPool::clear(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.entries.clear();
    pool.reported.clear();
    PhysicsMaterialRuntime::bumpRevision();
}

void PhysicsMaterialPool::clearUnused(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool removed = false;
    for (auto it = pool.entries.begin(); it != pool.entries.end();){
        if (it->second.use_count() <= 1){
            it = pool.entries.erase(it);
            removed = true;
        }else{
            ++it;
        }
    }
    if (removed){
        PhysicsMaterialRuntime::bumpRevision();
    }
}

size_t PhysicsMaterialPool::size(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.entries.size();
}
