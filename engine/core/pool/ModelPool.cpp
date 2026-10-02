// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelPool.h"
#include "ObjModelData.h"

#include "tiny_gltf.h"

#include <vector>

using namespace jasperengine;

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

std::shared_ptr<tinygltf::Model> ModelPool::getGLTF(const std::string& id){
    auto it = getMap().find(id);
    return it == getMap().end() ? nullptr : it->second;
}

std::shared_ptr<tinygltf::Model> ModelPool::addGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model){
    auto& shared = getMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

std::shared_ptr<ObjModelData> ModelPool::getObj(const std::string& id){
    auto it = getObjMap().find(id);
    return it == getObjMap().end() ? nullptr : it->second;
}

std::shared_ptr<ObjModelData> ModelPool::addObj(const std::string& id, std::shared_ptr<ObjModelData> model){
    auto& shared = getObjMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

void ModelPool::removeGLTF(const std::string& id){
    auto& map = getMap();
    auto it = map.find(id);
    if (it != map.end() && it->second.use_count() <= 1)
        map.erase(it);
}

void ModelPool::removeObj(const std::string& id){
    auto& map = getObjMap();
    auto it = map.find(id);
    if (it != map.end() && it->second.use_count() <= 1)
        map.erase(it);
}

void ModelPool::clear(){
    getMap().clear();
    getObjMap().clear();
}

void ModelPool::clearUnused(){
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
