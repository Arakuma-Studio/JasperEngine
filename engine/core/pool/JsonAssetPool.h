// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 7.2, 11.1)
// - Purpose: the cache behind AvatarMaskPool and AnimatorControllerPool: one immutable asset per
//   normalized path (AnimationClipPool's .anim key rule: FileData::getSystemPath, lexically
//   normal, generic separators), read through Data::open (resource packs work), with
//   AnimationClipPool's generation rules: a key's generation grows on every load after an
//   invalidate and never decreases; revision() grows on any change; failures are not cached.
// - Internal to the pools (not in JasperEngine.h).
// - Thread-safety: one mutex, never held while loading (look up, unlock, load, lock, insert); an
//   invalidate during a load keeps its result out of the cache.

#ifndef JSON_ASSET_POOL_H
#define JSON_ASSET_POOL_H

#include "io/Data.h"
#include "subsystem/MeshSystem.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine{

    struct ClipWarning;

    template<class T>
    class JsonAssetPool{
    public:
        // builds the asset from the file text; null + error when it is not one
        using Loader = std::function<std::shared_ptr<const T>(const std::string& text, std::string* error)>;

        explicit JsonAssetPool(Loader loader): loader(std::move(loader)){}

        static std::string key(const std::string& path){
            return path.empty() ? std::string() : MeshSystem::getModelFilenameKey(path);
        }

        std::shared_ptr<const T> get(const std::string& path){
            const std::string k = key(path);
            std::lock_guard<std::mutex> lock(mutex);
            auto it = entries.find(k);
            return it == entries.end() ? nullptr : it->second;
        }

        std::shared_ptr<const T> getOrLoad(const std::string& path, std::string* error){
            const std::string k = key(path);
            if (k.empty()){
                if (error) *error = "no path";
                return nullptr;
            }
            uint64_t epoch = 0;
            uint64_t cleared = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = entries.find(k);
                if (it != entries.end()) return it->second;
                epoch = epochs[k];
                cleared = clearEpoch;
            }
            std::shared_ptr<const T> asset;
            try{
                Data data;
                if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
                    if (error) *error = "cannot read " + path;
                    return nullptr;
                }
                std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
                asset = loader(text, error);
            }catch (const std::exception& e){
                if (error) *error = std::string("cannot load ") + path + ": " + e.what();
                return nullptr;
            }
            if (!asset) return nullptr;
            std::lock_guard<std::mutex> lock(mutex);
            auto it = entries.find(k);
            if (it != entries.end()) return it->second;
            if (epochs[k] != epoch || clearEpoch != cleared) return asset;
            entries[k] = asset;
            generations[k] = ++generationCounter;
            revisionCounter++;
            return asset;
        }

        uint64_t getGeneration(const std::string& path){
            const std::string k = key(path);
            std::lock_guard<std::mutex> lock(mutex);
            auto it = generations.find(k);
            return it == generations.end() ? 0 : it->second;
        }

        uint64_t revision(){
            std::lock_guard<std::mutex> lock(mutex);
            return revisionCounter;
        }

        void invalidate(const std::string& path){
            const std::string k = key(path);
            std::lock_guard<std::mutex> lock(mutex);
            epochs[k]++;
            entries.erase(k);
            auto generation = generations.find(k);
            if (generation != generations.end()) generation->second = ++generationCounter;
            revisionCounter++;
        }

        void clear(){
            std::lock_guard<std::mutex> lock(mutex);
            entries.clear();
            clearEpoch++;
            revisionCounter++;
        }

        void clearUnused(){
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = entries.begin(); it != entries.end();){
                if (it->second.use_count() <= 1) it = entries.erase(it);
                else ++it;
            }
        }

        size_t size(){
            std::lock_guard<std::mutex> lock(mutex);
            return entries.size();
        }

    private:
        Loader loader;
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<const T>> entries;
        std::map<std::string, uint64_t> generations;
        std::map<std::string, uint64_t> epochs;
        uint64_t generationCounter = 0;
        uint64_t clearEpoch = 0;
        uint64_t revisionCounter = 0;
    };

}

#endif // JSON_ASSET_POOL_H
