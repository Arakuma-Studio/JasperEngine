// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderVariantCollection.h"

#include "Scene.h"
#include "pool/ShaderPool.h"
#include "render/ShaderRender.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>

using namespace jasperengine;

ShaderVariantCollection::ShaderVariantCollection(){
}

ShaderVariantCollection::ShaderVariantCollection(const ShaderVariantCollection& other)
    : variants(other.variants), held(other.held){
}

ShaderVariantCollection& ShaderVariantCollection::operator=(const ShaderVariantCollection& other){
    if (this != &other){
        clear();
        variants = other.variants;
        held = other.held;
    }
    return *this;
}

ShaderVariantCollection::~ShaderVariantCollection(){
    clear();
}

void ShaderVariantCollection::releaseVariant(ShaderKey key){
    auto it = held.find(key);
    if (it == held.end())
        return;
    held.erase(it);
    // frees the program unless something else still draws with it
    ShaderPool::remove(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
}

bool ShaderVariantCollection::addKey(ShaderKey key){
    key = ShaderPool::normalizeKey(key);
    if (std::find(variants.begin(), variants.end(), key) != variants.end())
        return false;
    variants.push_back(key);
    return true;
}

bool ShaderVariantCollection::add(ShaderType type, uint32_t properties, const std::string& customShader){
    return addKey(ShaderPool::getShaderKey(type, properties, ShaderPool::registerCustomShader(customShader)));
}

bool ShaderVariantCollection::add(const std::string& spec, const std::string& customShader){
    ShaderKey key;
    if (!ShaderPool::parseShaderSpec(spec, key, nullptr, customShader))
        return false;
    return addKey(key);
}

size_t ShaderVariantCollection::addEntityVariants(Scene* scene, Entity entity){
    if (!scene)
        return 0;
    std::vector<ShaderKey> keys;
    scene->getSystem<RenderSystem>()->getShaderVariants(entity, keys);
    size_t added = 0;
    for (ShaderKey key : keys){
        if (addKey(key))
            added++;
    }
    return added;
}

bool ShaderVariantCollection::remove(ShaderType type, uint32_t properties, const std::string& customShader){
    const ShaderKey key = ShaderPool::normalizeKey(ShaderPool::getShaderKey(type, properties, ShaderPool::registerCustomShader(customShader)));
    auto it = std::find(variants.begin(), variants.end(), key);
    if (it == variants.end())
        return false;
    variants.erase(it);
    releaseVariant(key);
    return true;
}

bool ShaderVariantCollection::remove(const std::string& spec, const std::string& customShader){
    ShaderKey key;
    if (!ShaderPool::parseShaderSpec(spec, key, nullptr, customShader))
        return false;
    return remove(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key), customShader);
}

bool ShaderVariantCollection::contains(ShaderType type, uint32_t properties, const std::string& customShader) const{
    const ShaderKey key = ShaderPool::normalizeKey(ShaderPool::getShaderKey(type, properties, ShaderPool::registerCustomShader(customShader)));
    return std::find(variants.begin(), variants.end(), key) != variants.end();
}

bool ShaderVariantCollection::contains(const std::string& spec, const std::string& customShader) const{
    ShaderKey key;
    if (!ShaderPool::parseShaderSpec(spec, key, nullptr, customShader))
        return false;
    return std::find(variants.begin(), variants.end(), ShaderPool::normalizeKey(key)) != variants.end();
}

void ShaderVariantCollection::clear(){
    variants.clear();
    while (!held.empty())
        releaseVariant(held.begin()->first);
}

size_t ShaderVariantCollection::getVariantCount() const{
    return variants.size();
}

const std::vector<ShaderKey>& ShaderVariantCollection::getVariants() const{
    return variants;
}

bool ShaderVariantCollection::warmUp(){
    return warmUpProgressively(variants.size());
}

bool ShaderVariantCollection::warmUpProgressively(size_t maxRequests){
    size_t requests = 0;
    for (ShaderKey key : variants){
        const ResourceLoadState state = ShaderPool::getShaderState(key);
        if (state == ResourceLoadState::Failed)
            continue;
        // the ones in flight are picked up whatever the budget: that is no new work
        if (state == ResourceLoadState::NotStarted){
            if (requests >= maxRequests)
                continue;
            requests++;
        }
        if (ShaderPool::warmUp(key) && !held.count(key)){
            held[key] = ShaderPool::get(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
        }
    }
    return isWarmedUp();
}

bool ShaderVariantCollection::isWarmedUp() const{
    for (ShaderKey key : variants){
        const ResourceLoadState state = ShaderPool::getShaderState(key);
        if (state != ResourceLoadState::Finished && state != ResourceLoadState::Failed)
            return false;
    }
    return true;
}

size_t ShaderVariantCollection::getWarmedUpVariantCount() const{
    size_t count = 0;
    for (ShaderKey key : variants){
        if (ShaderPool::getShaderState(key) == ResourceLoadState::Finished)
            count++;
    }
    return count;
}

size_t ShaderVariantCollection::getFailedVariantCount() const{
    size_t count = 0;
    for (ShaderKey key : variants){
        if (ShaderPool::getShaderState(key) == ResourceLoadState::Failed)
            count++;
    }
    return count;
}
