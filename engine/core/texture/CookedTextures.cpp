// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CookedTextures.h"

#include "Log.h"
#include "System.h"
#include "io/Data.h"
#include "render/SystemRender.h"
#include "texture/TextureData.h"

#include "json.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace {
    struct CookedState {
        std::mutex mutex;
        CookedTextures::Provider provider;
        std::string providerFamily;
        CookedTextures::CpuReadHandler cpuReadHandler;
        bool loaded = false;
        std::string family;
        std::unordered_map<std::string, CookedTextureEntry> entries;
    };

    CookedState& cookedState(){
        static CookedState* s = new CookedState();
        return *s;
    }

    uint32_t parseCookedFlags(const nlohmann::json& flags){
        uint32_t out = 0;
        if (!flags.is_array())
            return out;
        for (const auto& flag : flags){
            if (!flag.is_string())
                continue;
            const std::string name = flag.get<std::string>();
            if (name == "alpha") out |= TEXTURE_FLAG_ALPHA;
            else if (name == "normalRG") out |= TEXTURE_FLAG_NORMAL_RG;
            else if (name == "coverage") out |= TEXTURE_FLAG_COVERAGE;
            else if (name == "displayEncoded") out |= TEXTURE_FLAG_DISPLAY_ENCODED;
            else if (name == "cpu") out |= TEXTURE_FLAG_CPU_READABLE;
            else if (name == "streamable") out |= TEXTURE_FLAG_STREAMABLE;
            else if (name == "cube") out |= TEXTURE_FLAG_CUBE;
        }
        return out;
    }

    bool readCookedIndex(const std::string& family, std::unordered_map<std::string, CookedTextureEntry>& out){
        const std::string dir = std::string(CookedTextures::kCookedDir) + "/" + family + "/";
        Data file;
        if (file.open((dir + "index.json").c_str()) != FileErrors::FILEDATA_OK)
            return false;
        const std::string text = file.readString();
        const nlohmann::json index = nlohmann::json::parse(text, nullptr, false);
        if (index.is_discarded() || !index.is_object() || !index.contains("textures") || !index["textures"].is_object()){
            Log::error("Cooked texture index %sindex.json is not valid", dir.c_str());
            return false;
        }
        for (const auto& [source, value] : index["textures"].items()){
            if (!value.is_object() || !value.contains("file"))
                continue;
            CookedTextureEntry entry;
            entry.file = dir + value.value("file", std::string());
            entry.width = value.value("width", 0);
            entry.height = value.value("height", 0);
            entry.format = value.value("format", std::string());
            entry.flags = value.contains("flags") ? parseCookedFlags(value["flags"]) : 0;
            entry.levels = value.value("levels", 1);
            entry.faces = value.value("faces", 1);
            entry.bytes = value.value("bytes", (uint64_t)0);
            entry.tailBytes = value.value("tail", (uint64_t)0);
            const std::string cpu = value.value("cpu", std::string());
            entry.cpuFile = cpu.empty() ? std::string() : dir + cpu;
            out[source] = std::move(entry);
        }
        return true;
    }

#if defined(__ANDROID__)
    bool cookedIndexExists(const std::string& family){
        Data file;
        return file.open((std::string(CookedTextures::kCookedDir) + "/" + family + "/index.json").c_str()) == FileErrors::FILEDATA_OK;
    }
#endif

    // first use: the build's family and its index (Android without ASTC takes android_etc2
    // when the export shipped it)
    void ensureCookedIndex(CookedState& s){
        if (s.loaded)
            return;
        s.loaded = true;
        s.family = CookedTextures::defaultFamily();
#if defined(__ANDROID__)
        if (!SystemRender::isTextureFormatSupported(GpuTextureFormat::ASTC_4x4) && cookedIndexExists("android_etc2")){
            Log::print("This GPU has no ASTC: loading the android_etc2 textures");
            s.family = "android_etc2";
        }
#endif
        readCookedIndex(s.family, s.entries);
    }
}

std::string CookedTextures::defaultFamily(){
#if defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__ANDROID__)
    return "mobile";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "mobile";
#else
    return "desktop";
#endif
}

std::string CookedTextures::activeFamily(){
    CookedState& s = cookedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.provider)
        return s.providerFamily;
    ensureCookedIndex(s);
    return s.family;
}

bool CookedTextures::find(const std::string& sourcePath, CookedTextureEntry& out){
    const std::string key = normalize(sourcePath);
    if (key.empty())
        return false;
    CookedState& s = cookedState();
    Provider provider;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.provider){
            ensureCookedIndex(s);
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

void CookedTextures::setProvider(Provider provider, const std::string& family){
    CookedState& s = cookedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.provider = std::move(provider);
    s.providerFamily = family;
}

void CookedTextures::setCpuReadHandler(CpuReadHandler handler){
    CookedState& s = cookedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cpuReadHandler = std::move(handler);
}

void CookedTextures::requireCpuReadable(const std::string& sourcePath){
    const std::string key = normalize(sourcePath);
    if (key.empty())
        return;
    CpuReadHandler handler;
    {
        CookedState& s = cookedState();
        std::lock_guard<std::mutex> lock(s.mutex);
        handler = s.cpuReadHandler;
    }
    // outside the lock, as find() calls the provider
    if (handler)
        handler(key);
}

bool CookedTextures::hasProvider(){
    CookedState& s = cookedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return (bool)s.provider;
}

std::string CookedTextures::normalize(const std::string& path){
    std::string p = TextureData::parseSvgScalePath(path);
    for (char& c : p)
        if (c == '\\') c = '/';
    if (p.rfind("asset://", 0) == 0){
        p = p.substr(8);
    }else if (!p.empty() && (p[0] == '/' || (p.size() > 1 && p[1] == ':'))){
        std::string assets = System::instance().getAssetPath();
        for (char& c : assets)
            if (c == '\\') c = '/';
        if (!assets.empty() && assets.back() != '/')
            assets += '/';
        if (assets.size() <= 1 || p.rfind(assets, 0) != 0)
            return std::string();
        p = p.substr(assets.size());
    }else if (p.find("://") != std::string::npos){
        return std::string();   // data://, lua://, user files: never cooked
    }
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= p.size()){
        size_t end = p.find('/', at);
        if (end == std::string::npos)
            end = p.size();
        const std::string part = p.substr(at, end - at);
        if (part == ".."){
            if (!parts.empty())
                parts.pop_back();
        }else if (!part.empty() && part != "."){
            parts.push_back(part);
        }
        at = end + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++){
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

void CookedTextures::reset(){
    CookedState& s = cookedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.loaded = false;
    s.entries.clear();
}
