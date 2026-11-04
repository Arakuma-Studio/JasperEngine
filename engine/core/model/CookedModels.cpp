// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CookedModels.h"

#include "Log.h"
#include "io/Data.h"
#include "texture/CookedTextures.h"

#include "json.hpp"

#include <mutex>
#include <unordered_map>

using namespace jasperengine;

namespace {
    struct CookedModelState {
        std::mutex mutex;
        CookedModels::Provider provider;
        bool loaded = false;
        std::unordered_map<std::string, CookedModelEntry> entries;
    };

    CookedModelState& cookedModelState(){
        static CookedModelState* s = new CookedModelState();
        return *s;
    }

    // first use: this build's family of the export's index (none: every model loads raw)
    void ensureModelIndex(CookedModelState& s){
        if (s.loaded)
            return;
        s.loaded = true;
        Data file;
        if (file.open(CookedModels::kIndexFile) != FileErrors::FILEDATA_OK)
            return;
        const nlohmann::json index = nlohmann::json::parse(file.readString(), nullptr, false);
        if (index.is_discarded() || !index.is_object() || !index.contains("models") || !index["models"].is_object()){
            Log::error("Cooked model index %s is not valid", CookedModels::kIndexFile);
            return;
        }
        const int format = index.value("format", 0);
        if (format > CookedModels::kFormatVersion){
            Log::error("Cooked models are of format %d, this player reads %d: they load from their sources", format,
                       CookedModels::kFormatVersion);
            return;
        }
        // the textures' family names: desktop, mobile, web (android_etc2 takes mobile's models)
        std::string family = CookedTextures::activeFamily();
        if (family == "android_etc2")
            family = "mobile";
        const std::string dir = std::string(CookedModels::kCookedDir) + "/";
        for (const auto& [source, families] : index["models"].items()){
            if (!families.is_object() || !families.contains(family) || !families[family].is_object())
                continue;
            const nlohmann::json& value = families[family];
            CookedModelEntry entry;
            entry.file = dir + value.value("file", std::string());
            entry.bytes = value.value("bytes", (uint64_t)0);
            entry.sourceBytes = value.value("sourceBytes", (uint64_t)0);
            entry.keepCpuData = value.value("keepCpuData", true);
            if (entry.file.size() > dir.size())
                s.entries[source] = std::move(entry);
        }
    }
}

bool CookedModels::find(const std::string& sourcePath, CookedModelEntry& out){
    const std::string key = CookedTextures::normalize(sourcePath);
    if (key.empty())
        return false;
    CookedModelState& s = cookedModelState();
    Provider provider;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.provider){
            ensureModelIndex(s);
            auto it = s.entries.find(key);
            if (it == s.entries.end())
                return false;
            out = it->second;
            return true;
        }
        provider = s.provider;
    }
    // outside the lock: the editor's provider takes its own
    return provider(key, out);
}

void CookedModels::setProvider(Provider provider){
    CookedModelState& s = cookedModelState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.provider = std::move(provider);
}

bool CookedModels::hasProvider(){
    CookedModelState& s = cookedModelState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return (bool)s.provider;
}

void CookedModels::reset(){
    CookedModelState& s = cookedModelState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.loaded = false;
    s.entries.clear();
}
