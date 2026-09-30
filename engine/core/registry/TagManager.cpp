// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "TagManager.h"
#include "ecs/EntityManager.h"
#include "Log.h"

#include <algorithm>
#include <array>

using namespace jasperengine;

namespace{

    struct TagData{
        std::vector<std::string> tags;
        std::array<std::string, MAX_LAYERS> layers;
    };

    // function-local, so a scene built during static initialization still sees it
    TagData& tagData(){
        static TagData data;
        return data;
    }

    const char* const builtinLayerNames[MAX_LAYERS] = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI"
    };

}

const std::vector<std::string>& TagManager::getBuiltinTags(){
    static const std::vector<std::string> tags = {
        DEFAULT_ENTITY_TAG, "Respawn", "Finish", "EditorOnly", "MainCamera", "Player", "GameController"
    };
    return tags;
}

bool TagManager::isBuiltinTag(const std::string& tag){
    const std::vector<std::string>& builtin = getBuiltinTags();
    return std::find(builtin.begin(), builtin.end(), tag) != builtin.end();
}

bool TagManager::isBuiltinLayer(int layer){
    return layer >= 0 && layer < MAX_LAYERS && builtinLayerNames[layer] && builtinLayerNames[layer][0] != '\0';
}

const char* TagManager::getBuiltinLayerName(int layer){
    if (layer < 0 || layer >= MAX_LAYERS || !builtinLayerNames[layer])
        return "";
    return builtinLayerNames[layer];
}

std::string TagManager::trimName(const std::string& name){
    const char* blanks = " \t\r\n\v\f";
    size_t first = name.find_first_not_of(blanks);
    if (first == std::string::npos)
        return std::string();
    size_t last = name.find_last_not_of(blanks);
    return name.substr(first, last - first + 1);
}

void TagManager::setTags(const std::vector<std::string>& tags){
    std::vector<std::string> accepted;
    accepted.reserve(tags.size());
    for (const std::string& raw : tags){
        std::string tag = trimName(raw);
        if (tag.empty() || isBuiltinTag(tag) || std::find(accepted.begin(), accepted.end(), tag) != accepted.end()){
            Log::warn("Tag \"%s\" is empty or already defined, skipped", raw.c_str());
            continue;
        }
        accepted.push_back(std::move(tag));
    }
    tagData().tags = std::move(accepted);
}

const std::vector<std::string>& TagManager::getTags(){
    return tagData().tags;
}

std::vector<std::string> TagManager::getAllTags(){
    std::vector<std::string> all = getBuiltinTags();
    const std::vector<std::string>& user = tagData().tags;
    all.insert(all.end(), user.begin(), user.end());
    return all;
}

bool TagManager::isTagDefined(const std::string& tag){
    if (isBuiltinTag(tag))
        return true;
    const std::vector<std::string>& user = tagData().tags;
    return std::find(user.begin(), user.end(), tag) != user.end();
}

bool TagManager::setLayerName(int layer, const std::string& name){
    if (layer < 0 || layer >= MAX_LAYERS){
        Log::error("Layer %i is out of range (0 to 31)", layer);
        return false;
    }
    if (isBuiltinLayer(layer)){
        Log::error("Layer %i is a built-in layer, its name cannot change", layer);
        return false;
    }

    std::string trimmed = trimName(name);
    if (!trimmed.empty()){
        int existing = nameToLayer(trimmed);
        if (existing >= 0 && existing != layer){
            Log::error("Layer name \"%s\" is already used by layer %i", trimmed.c_str(), existing);
            return false;
        }
    }

    tagData().layers[layer] = std::move(trimmed);
    return true;
}

void TagManager::setLayerNames(const std::vector<std::string>& names){
    for (std::string& layer : tagData().layers){
        layer.clear();
    }
    const int count = std::min((int)names.size(), MAX_LAYERS);
    for (int layer = 0; layer < count; layer++){
        if (isBuiltinLayer(layer) || names[layer].empty())
            continue;
        setLayerName(layer, names[layer]);
    }
}

std::string TagManager::layerToName(int layer){
    if (layer < 0 || layer >= MAX_LAYERS)
        return std::string();
    if (isBuiltinLayer(layer))
        return builtinLayerNames[layer];
    return tagData().layers[layer];
}

int TagManager::nameToLayer(const std::string& name){
    if (name.empty())
        return -1;
    const TagData& data = tagData();
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        if (isBuiltinLayer(layer) ? name == builtinLayerNames[layer] : name == data.layers[layer])
            return layer;
    }
    return -1;
}

uint32_t TagManager::getMask(const std::vector<std::string>& layerNames){
    uint32_t mask = LAYER_MASK_NOTHING;
    for (const std::string& name : layerNames){
        int layer = nameToLayer(name);
        if (layer >= 0)
            mask |= (1u << layer);
    }
    return mask;
}

void TagManager::reset(){
    TagData& data = tagData();
    data.tags.clear();
    for (std::string& layer : data.layers){
        layer.clear();
    }
}
