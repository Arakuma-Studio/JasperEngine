// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SortingLayers.h"
#include "TagManager.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{
    constexpr int32_t MAX_SORTING_LAYER_ID = 1 << 20;
}

SortingLayers::Table& SortingLayers::table(){
    static Table data = [](){
        Table t;
        t.layers.push_back({DEFAULT_SORTING_LAYER_ID, "Default"});
        rebuildValues(t);
        return t;
    }();
    return data;
}

void SortingLayers::rebuildValues(Table& t){
    int32_t maxId = 0;
    for (const SortingLayerInfo& layer : t.layers)
        maxId = std::max(maxId, layer.id);
    t.defaultValue = 0;
    for (size_t i = 0; i < t.layers.size(); i++){
        if (t.layers[i].id == DEFAULT_SORTING_LAYER_ID)
            t.defaultValue = (uint16_t)i;
    }
    t.values.assign((size_t)maxId + 1, t.defaultValue);
    for (size_t i = 0; i < t.layers.size(); i++)
        t.values[(size_t)t.layers[i].id] = (uint16_t)i;
}

void SortingLayers::setLayers(const std::vector<SortingLayerInfo>& layers){
    std::vector<SortingLayerInfo> accepted;
    accepted.reserve(layers.size() + 1);
    bool hasDefault = false;
    for (const SortingLayerInfo& raw : layers){
        SortingLayerInfo layer{raw.id, TagManager::trimName(raw.name)};
        if (layer.id == DEFAULT_SORTING_LAYER_ID){
            if (hasDefault)
                continue;
            hasDefault = true;
            layer.name = "Default";
            accepted.push_back(layer);
            continue;
        }
        // (65535 layers at most: the value is 16 bits of the sort key; ids index a flat table)
        const bool repeated = std::any_of(accepted.begin(), accepted.end(), [&](const SortingLayerInfo& other){
            return other.id == layer.id || other.name == layer.name;
        });
        if (layer.id < 0 || layer.id > MAX_SORTING_LAYER_ID || layer.name.empty() || layer.name == "Default" || repeated || accepted.size() >= 65535){
            Log::warn("Sorting Layer \"%s\" (id %i) is empty, repeated or out of range, skipped", raw.name.c_str(), (int)raw.id);
            continue;
        }
        accepted.push_back(std::move(layer));
    }
    if (!hasDefault)
        accepted.insert(accepted.begin(), SortingLayerInfo{DEFAULT_SORTING_LAYER_ID, "Default"});

    Table& t = table();
    if (accepted == t.layers)
        return;
    t.layers = std::move(accepted);
    rebuildValues(t);
    t.revision++;
}

const std::vector<SortingLayerInfo>& SortingLayers::getLayers(){
    return table().layers;
}

std::vector<SortingLayerInfo> SortingLayers::fromNames(const std::vector<std::string>& names){
    const std::vector<SortingLayerInfo>& current = table().layers;
    int32_t nextId = 1;
    for (const SortingLayerInfo& layer : current)
        nextId = std::max(nextId, layer.id + 1);
    std::vector<SortingLayerInfo> result;
    result.reserve(names.size() + 1);
    for (const std::string& raw : names){
        const std::string name = TagManager::trimName(raw);
        if (name.empty())
            continue;
        if (name == "Default"){
            result.push_back({DEFAULT_SORTING_LAYER_ID, name});
            continue;
        }
        const int32_t existing = nameToId(name);
        result.push_back({existing >= 0 ? existing : nextId++, name});
    }
    return result;
}

int32_t SortingLayers::nameToId(const std::string& name){
    for (const SortingLayerInfo& layer : table().layers){
        if (layer.name == name)
            return layer.id;
    }
    return -1;
}

std::string SortingLayers::idToName(int32_t id){
    for (const SortingLayerInfo& layer : table().layers){
        if (layer.id == id)
            return layer.name;
    }
    return std::string();
}

bool SortingLayers::isValid(int32_t id){
    for (const SortingLayerInfo& layer : table().layers){
        if (layer.id == id)
            return true;
    }
    return false;
}

int SortingLayers::getLayerValue(int32_t id){
    return (int)valueOf(id);
}

void SortingLayers::setTransparencySortMode(TransparencySortMode mode){
    Table& t = table();
    if (t.mode != mode){
        t.mode = mode;
        t.revision++;
    }
}

TransparencySortMode SortingLayers::getTransparencySortMode(){
    return table().mode;
}

void SortingLayers::setTransparencySortAxis(const Vector3& axis){
    Table& t = table();
    const Vector3 finite(std::isfinite(axis.x) ? axis.x : 0.0f, std::isfinite(axis.y) ? axis.y : 0.0f, std::isfinite(axis.z) ? axis.z : 0.0f);
    if (t.axis != finite){
        t.axis = finite;
        t.revision++;
    }
}

Vector3 SortingLayers::getTransparencySortAxis(){
    return table().axis;
}

uint32_t SortingLayers::getRevision(){
    return table().revision;
}

void SortingLayers::reset(){
    setLayers({});
    setTransparencySortMode(TransparencySortMode::DEFAULT);
    setTransparencySortAxis(Vector3(0.0f, 1.0f, 0.0f));
}
