// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImportedAssets.h"

#include "Log.h"
#include "io/Data.h"
#include "texture/CookedTextures.h"

#include "json.hpp"

#include <mutex>
#include <unordered_map>

using namespace jasperengine;

namespace {

    struct ImportedState {
        std::mutex mutex;
        ImportedAssets::Provider provider;
        bool loaded = false;
        std::unordered_map<std::string, std::string> files;
    };

    ImportedState& importedState(){
        static ImportedState* s = new ImportedState();
        return *s;
    }

    void ensureIndex(ImportedState& s){
        if (s.loaded)
            return;
        s.loaded = true;
        const std::string dir = std::string(ImportedAssets::kImportedDir) + "/";
        Data file;
        if (file.open((dir + "index.json").c_str()) != FileErrors::FILEDATA_OK)
            return;   // a game without imported assets
        const std::string text = file.readString();
        const nlohmann::json index = nlohmann::json::parse(text, nullptr, false);
        if (index.is_discarded() || !index.is_object() || index.value("format", std::string()) != ImportedAssets::kIndexFormat ||
            !index.contains("files") || !index["files"].is_object()){
            Log::error("Imported asset index %sindex.json is not valid", dir.c_str());
            return;
        }
        if (index.value("version", 0) > ImportedAssets::kIndexVersion){
            Log::error("Imported asset index %sindex.json is version %d, newer than this engine reads (%d)", dir.c_str(),
                       index.value("version", 0), ImportedAssets::kIndexVersion);
            return;
        }
        for (const auto& [ref, value] : index["files"].items()){
            if (value.is_string())
                s.files[ref] = value.get<std::string>();
        }
    }

}

std::string ImportedAssets::normalize(const std::string& sourceRef){
    const size_t hash = sourceRef.find('#');
    if (hash == std::string::npos)
        return CookedTextures::normalize(sourceRef);
    const std::string source = CookedTextures::normalize(sourceRef.substr(0, hash));
    if (source.empty())
        return std::string();
    return source + sourceRef.substr(hash);
}

std::string ImportedAssets::reference(const std::string& source, const std::string& key){
    return source + "#" + key;
}

bool ImportedAssets::find(const std::string& sourceRef, std::string& fileOut){
    const std::string key = normalize(sourceRef);
    if (key.empty() || key.find('#') == std::string::npos)
        return false;
    ImportedState& s = importedState();
    Provider provider;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.provider){
            ensureIndex(s);
            auto it = s.files.find(key);
            if (it == s.files.end())
                return false;
            fileOut = it->second;
            return true;
        }
        provider = s.provider;
    }
    // outside the lock: the editor's provider takes its own
    return provider(key, fileOut);
}

void ImportedAssets::setProvider(Provider provider){
    ImportedState& s = importedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.provider = std::move(provider);
}

bool ImportedAssets::hasProvider(){
    ImportedState& s = importedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return (bool)s.provider;
}

void ImportedAssets::reset(){
    ImportedState& s = importedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.loaded = false;
    s.files.clear();
}
