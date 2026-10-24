// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipPool.h"

#include "animation/AnimationClip.h"

using namespace jasperengine;

std::string AnimationClipPool::getKey(const std::string& path){
    // TODO(T1a): normalized key (MeshSystem::getModelFilenameKey style; sub-clips by index)
    return path;
}

std::shared_ptr<const AnimationClip> AnimationClipPool::get(const std::string& path){
    // TODO(T1a)
    (void)path;
    return nullptr;
}

std::shared_ptr<const AnimationClip> AnimationClipPool::getOrLoad(const std::string& path, std::string* error,
                                                                  std::vector<ClipWarning>* warnings){
    // TODO(T1a): Data::open + anim::parseClip + AnimationClip::build; T1c: sub-clips
    (void)path;
    (void)warnings;
    if (error) *error = "Animation clip loading is not implemented yet.";
    return nullptr;
}

uint64_t AnimationClipPool::getGeneration(const std::string& path){
    // TODO(T1a)
    (void)path;
    return 0;
}

uint64_t AnimationClipPool::revision(){
    // TODO(T1a)
    return 0;
}

void AnimationClipPool::invalidate(const std::string& path){
    // TODO(T1a)
    (void)path;
}

void AnimationClipPool::invalidateModel(const std::string& modelPath){
    // TODO(T1a)
    (void)modelPath;
}

void AnimationClipPool::clear(){
    // TODO(T1a)
}

void AnimationClipPool::clearUnused(){
    // TODO(T1a)
}

size_t AnimationClipPool::size(){
    // TODO(T1a)
    return 0;
}
