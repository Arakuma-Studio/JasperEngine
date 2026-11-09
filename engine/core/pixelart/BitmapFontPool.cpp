// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapFontPool.h"

#include "io/FileData.h"

#include <filesystem>
#include <map>

namespace jasperengine::pixelart {

    namespace{
        struct PoolState {
            std::map<std::string, std::shared_ptr<BitmapFont>> fonts;   // by path
            std::map<std::string, std::shared_ptr<BitmapFont>> chains;  // by chain key
            std::map<std::string, uint32_t> generations;
            uint32_t revision = 1;
        };

        PoolState& state(){
            static PoolState pool;
            return pool;
        }

        // a path as the file system sees it: engine paths (relative to the assets, asset://) and
        // the watcher's absolute paths compare equal
        std::string normalized(const std::string& path){
            return std::filesystem::path(FileData::getSystemPath(path)).lexically_normal().generic_string();
        }

        bool reads(const BitmapFont& font, const std::string& wanted){
            const std::string target = normalized(wanted);
            for (const std::string& dependency : font.getDependencies()){
                if (dependency == wanted || normalized(dependency) == target)
                    return true;
            }
            return false;
        }
    }

    std::shared_ptr<BitmapFont> BitmapFontPool::get(const std::string& path, BitmapFontReport* report){
        PoolState& pool = state();
        auto it = pool.fonts.find(path);
        if (it != pool.fonts.end()){
            if (report) *report = it->second->getReport();
            return it->second;
        }
        std::shared_ptr<BitmapFont> font = BitmapFont::load(path, report);
        if (font)
            pool.fonts[path] = font;
        return font;
    }

    std::string BitmapFontPool::chainKey(const std::vector<std::string>& paths){
        std::string key;
        for (const std::string& path : paths){
            if (path.empty()) continue;
            key += (key.empty() ? "" : ";") + path;
        }
        return key;
    }

    std::shared_ptr<BitmapFont> BitmapFontPool::getChain(const std::vector<std::string>& paths, std::vector<std::string>* skipped, std::string* keyOut){
        std::vector<std::string> list;
        for (const std::string& path : paths)
            if (!path.empty()) list.push_back(path);
        if (list.empty())
            return nullptr;
        std::shared_ptr<BitmapFont> primary = get(list[0]);
        if (!primary)
            return nullptr;
        // a vector fallback never mixes into bitmap text (R14): skipped, S42 reports it
        std::vector<std::string> used = {list[0]};
        std::vector<std::shared_ptr<BitmapFont>> others;
        for (size_t i = 1; i < list.size(); i++){
            std::shared_ptr<BitmapFont> other = isBitmapFontPath(list[i]) ? get(list[i]) : nullptr;
            if (!other || other->getNativeSize() != primary->getNativeSize()){
                if (skipped) skipped->push_back(list[i]);
                continue;
            }
            used.push_back(list[i]);
            others.push_back(other);
        }
        const std::string key = chainKey(used);
        if (keyOut)
            *keyOut = key;
        if (others.empty())
            return primary;
        PoolState& pool = state();
        auto it = pool.chains.find(key);
        if (it != pool.chains.end())
            return it->second;
        std::shared_ptr<BitmapFont> chain = BitmapFont::merged(primary, others);
        pool.chains[key] = chain;
        return chain;
    }

    uint32_t BitmapFontPool::generation(const std::string& key){
        auto it = state().generations.find(key);
        return it == state().generations.end() ? 0 : it->second;
    }

    uint32_t BitmapFontPool::revision(){
        return state().revision;
    }

    std::vector<std::string> BitmapFontPool::invalidate(const std::string& file){
        PoolState& pool = state();
        std::vector<std::string> keys;
        for (auto it = pool.fonts.begin(); it != pool.fonts.end();){
            if (reads(*it->second, file) || it->first == file || normalized(it->first) == normalized(file)){
                keys.push_back(it->first);
                it = pool.fonts.erase(it);
            }else{
                ++it;
            }
        }
        for (auto it = pool.chains.begin(); it != pool.chains.end();){
            bool drop = reads(*it->second, file);
            for (const std::string& key : keys)
                drop = drop || it->first == key || it->first.rfind(key + ";", 0) == 0 || it->first.find(";" + key) != std::string::npos;
            if (drop){
                keys.push_back(it->first);
                it = pool.chains.erase(it);
            }else{
                ++it;
            }
        }
        for (const std::string& key : keys)
            pool.generations[key]++;
        if (!keys.empty())
            pool.revision++;
        return keys;
    }

    void BitmapFontPool::clear(){
        state().fonts.clear();
        state().chains.clear();
    }

    void BitmapFontPool::clearUnused(){
        PoolState& pool = state();
        for (auto it = pool.chains.begin(); it != pool.chains.end();)
            it = (it->second.use_count() <= 1) ? pool.chains.erase(it) : std::next(it);
        for (auto it = pool.fonts.begin(); it != pool.fonts.end();)
            it = (it->second.use_count() <= 1) ? pool.fonts.erase(it) : std::next(it);
    }

}
