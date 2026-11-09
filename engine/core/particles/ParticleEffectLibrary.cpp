// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectLibrary.h"

#include "Log.h"
#include "io/Data.h"
#include "particles/ParticleEffectCodec.h"

#include <map>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    struct Library {
        std::map<std::string, EffectAsset> assets;
        uint64_t nextVersion = 1;
    };

    Library& library(){
        static Library* instance = new Library();
        return *instance;
    }

    void readInto(const std::string& path, EffectAsset& asset){
        asset.document.reset();
        asset.diagnostics.clear();
        asset.version = library().nextVersion++;
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            asset.diagnostics.push_back({Diagnostic::Severity::Error, "$", "cannot read " + path});
            Log::error("[Particles] Cannot read the effect %s", path.c_str());
            return;
        }
        ParticleEffectDocument document;
        if (!ParticleEffectCodec::read(data.readString(), document, asset.diagnostics)){
            Log::error("[Particles] %s: %s", path.c_str(), ParticleEffectCodec::format(asset.diagnostics).c_str());
            return;
        }
        asset.document = std::make_shared<const ParticleEffectDocument>(std::move(document));
    }

}

const EffectAsset& ParticleEffectLibrary::get(const std::string& path){
    Library& lib = library();
    auto it = lib.assets.find(path);
    if (it != lib.assets.end())
        return it->second;
    EffectAsset& asset = lib.assets[path];
    readInto(path, asset);
    return asset;
}

void ParticleEffectLibrary::reload(const std::string& path){
    readInto(path, library().assets[path]);
}

void ParticleEffectLibrary::set(const std::string& path, const ParticleEffectDocument& document){
    Library& lib = library();
    EffectAsset& asset = lib.assets[path];
    asset.document = std::make_shared<const ParticleEffectDocument>(document);
    asset.diagnostics.clear();
    ParticleEffectCodec::validate(document, asset.diagnostics);
    asset.version = lib.nextVersion++;
}

void ParticleEffectLibrary::clear(){
    library().assets.clear();
    library().nextVersion++;
}

uint64_t ParticleEffectLibrary::generation(){
    return library().nextVersion;
}

uint64_t ParticleEffectLibrary::version(const std::string& path){
    const Library& lib = library();
    auto it = lib.assets.find(path);
    return it != lib.assets.end() ? it->second.version : 0;
}
