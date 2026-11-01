// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AvatarMaskPool.h"

#include "animation/AvatarMask.h"
#include "pool/JsonAssetPool.h"

using namespace jasperengine;

namespace{

    // never destroyed: players may release masks during static destruction
    JsonAssetPool<anim::AvatarMask>& pool(){
        static JsonAssetPool<anim::AvatarMask>* instance = new JsonAssetPool<anim::AvatarMask>(
            [](const std::string& text, std::string* error) -> std::shared_ptr<const anim::AvatarMask> {
                anim::AvatarMaskParseResult parsed = anim::parseAvatarMask(text);
                if (!parsed.ok()){
                    if (error) *error = parsed.error.toString();
                    return nullptr;
                }
                return anim::AvatarMask::build(*parsed.data, parsed.warnings);
            });
        return *instance;
    }

}

std::string AvatarMaskPool::getKey(const std::string& path){
    return JsonAssetPool<anim::AvatarMask>::key(path);
}

std::shared_ptr<const anim::AvatarMask> AvatarMaskPool::get(const std::string& path){
    return pool().get(path);
}

std::shared_ptr<const anim::AvatarMask> AvatarMaskPool::getOrLoad(const std::string& path, std::string* error){
    return pool().getOrLoad(path, error);
}

uint64_t AvatarMaskPool::getGeneration(const std::string& path){
    return pool().getGeneration(path);
}

uint64_t AvatarMaskPool::revision(){
    return pool().revision();
}

void AvatarMaskPool::invalidate(const std::string& path){
    pool().invalidate(path);
}

void AvatarMaskPool::clear(){
    pool().clear();
}

void AvatarMaskPool::clearUnused(){
    pool().clearUnused();
}

size_t AvatarMaskPool::size(){
    return pool().size();
}
