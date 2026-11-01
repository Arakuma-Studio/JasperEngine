// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorControllerPool.h"

#include "animation/AnimatorController.h"
#include "pool/JsonAssetPool.h"

using namespace jasperengine;

namespace{

    // never destroyed: players may release controllers during static destruction
    JsonAssetPool<anim::AnimatorController>& pool(){
        static JsonAssetPool<anim::AnimatorController>* instance = new JsonAssetPool<anim::AnimatorController>(
            [](const std::string& text, std::string* error) -> std::shared_ptr<const anim::AnimatorController> {
                anim::ControllerParseResult parsed = anim::parseController(text);
                if (!parsed.ok()){
                    if (error) *error = parsed.error.toString();
                    return nullptr;
                }
                return anim::AnimatorController::build(*parsed.data, parsed.warnings);
            });
        return *instance;
    }

}

std::string AnimatorControllerPool::getKey(const std::string& path){
    return JsonAssetPool<anim::AnimatorController>::key(path);
}

std::shared_ptr<const anim::AnimatorController> AnimatorControllerPool::get(const std::string& path){
    return pool().get(path);
}

std::shared_ptr<const anim::AnimatorController> AnimatorControllerPool::getOrLoad(const std::string& path, std::string* error){
    return pool().getOrLoad(path, error);
}

uint64_t AnimatorControllerPool::getGeneration(const std::string& path){
    return pool().getGeneration(path);
}

uint64_t AnimatorControllerPool::revision(){
    return pool().revision();
}

void AnimatorControllerPool::invalidate(const std::string& path){
    pool().invalidate(path);
}

void AnimatorControllerPool::clear(){
    pool().clear();
}

void AnimatorControllerPool::clearUnused(){
    pool().clearUnused();
}

size_t AnimatorControllerPool::size(){
    return pool().size();
}
