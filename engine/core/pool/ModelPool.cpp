// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelPool.h"
#include "model/ModelCpuData.h"
#include "ObjModelData.h"

#include "tiny_gltf.h"

#include <mutex>
#include <vector>

using namespace jasperengine;

// A game's loading thread (Engine.startAsyncThread) loads models next to the main thread
static std::recursive_mutex& getPoolMutex(){
    static std::recursive_mutex mutex;
    return mutex;
}

models_t& ModelPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static models_t* map = new models_t();
    return *map;
}

obj_models_t& ModelPool::getObjMap(){
    static obj_models_t* map = new obj_models_t();
    return *map;
}

std::map<std::string, uint32_t>& ModelPool::getGenerations(){
    static auto* generations = new std::map<std::string, uint32_t>();
    return *generations;
}

void ModelPool::replaceGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model){
    if (!model)
        return;
    getMap()[id] = std::move(model);
    getGenerations()[id]++;
}

void ModelPool::replaceObj(const std::string& id, std::shared_ptr<ObjModelData> model){
    if (!model)
        return;
    getObjMap()[id] = std::move(model);
    getGenerations()[id]++;
}

uint32_t ModelPool::generation(const std::string& id){
    auto it = getGenerations().find(id);
    return it == getGenerations().end() ? 0 : it->second;
}

std::shared_ptr<tinygltf::Model> ModelPool::getGLTF(const std::string& id){
    std::shared_ptr<tinygltf::Model> model = peekGLTF(id);
    ModelCpuData::ensure(model);
    return model;
}

std::shared_ptr<tinygltf::Model> ModelPool::peekGLTF(const std::string& id){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto it = getMap().find(id);
    return it == getMap().end() ? nullptr : it->second;
}

std::shared_ptr<tinygltf::Model> ModelPool::addGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto& shared = getMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

std::shared_ptr<ObjModelData> ModelPool::getObj(const std::string& id){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto it = getObjMap().find(id);
    return it == getObjMap().end() ? nullptr : it->second;
}

std::shared_ptr<ObjModelData> ModelPool::addObj(const std::string& id, std::shared_ptr<ObjModelData> model){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto& shared = getObjMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

void ModelPool::removeGLTF(const std::string& id){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto& map = getMap();
    auto it = map.find(id);
    if (it != map.end() && it->second.use_count() <= 1)
        map.erase(it);
}

void ModelPool::removeObj(const std::string& id){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    auto& map = getObjMap();
    auto it = map.find(id);
    if (it != map.end() && it->second.use_count() <= 1)
        map.erase(it);
}

void ModelPool::clear(){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    getMap().clear();
    getObjMap().clear();
}

void ModelPool::invalidate(const std::string& id){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    getMap().erase(id);
    getObjMap().erase(id);
}

void ModelPool::clearUnused(){
    std::lock_guard<std::recursive_mutex> lock(getPoolMutex());
    std::vector<std::string> gltfIds;
    gltfIds.reserve(getMap().size());
    for (const auto& [id, _] : getMap()) {
        gltfIds.push_back(id);
    }
    for (const auto& id : gltfIds) {
        removeGLTF(id);
    }

    std::vector<std::string> objIds;
    objIds.reserve(getObjMap().size());
    for (const auto& [id, _] : getObjMap()) {
        objIds.push_back(id);
    }
    for (const auto& id : objIds) {
        removeObj(id);
    }
}
