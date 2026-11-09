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
    t.nextId = std::max(t.nextId, maxId + 1);
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
    std::vector<SortingLayerInfo> entries;
    entries.reserve(names.size());
    for (const std::string& name : names)
        entries.push_back({-1, name});
    std::vector<SortingLayerInfo> result;
    std::string error;
    resolve(entries, result, error);
    return result;
}

bool SortingLayers::resolve(const std::vector<SortingLayerInfo>& entries, std::vector<SortingLayerInfo>& out, std::string& error){
    out.clear();
    out.reserve(entries.size() + 1);
    // the ids the entries name keep their layers (a rename); a name then keeps its layer only
    // when no entry claimed that id
    std::vector<int32_t> claimed;
    for (const SortingLayerInfo& entry : entries){
        if (entry.id < 0)
            continue;
        if (!isValid(entry.id)){
            error = "no Sorting Layer has id " + std::to_string(entry.id) + " (leave the id out for a new layer)";
            return false;
        }
        claimed.push_back(entry.id);
    }
    int32_t nextId = getNextId();
    for (const SortingLayerInfo& entry : entries){
        const std::string name = TagManager::trimName(entry.name);
        if (name.empty())
            continue;
        if (entry.id >= 0){
            out.push_back({entry.id, entry.id == DEFAULT_SORTING_LAYER_ID ? std::string("Default") : name});
            continue;
        }
        if (name == "Default"){
            out.push_back({DEFAULT_SORTING_LAYER_ID, name});
            continue;
        }
        const int32_t existing = nameToId(name);
        const bool free = existing >= 0 && std::find(claimed.begin(), claimed.end(), existing) == claimed.end();
        out.push_back({free ? existing : nextId++, name});
    }
    return true;
}

int32_t SortingLayers::getNextId(){
    return table().nextId;
}

void SortingLayers::setNextId(int32_t id){
    Table& t = table();
    int32_t maxId = 0;
    for (const SortingLayerInfo& layer : t.layers)
        maxId = std::max(maxId, layer.id);
    t.nextId = std::clamp(id, maxId + 1, MAX_SORTING_LAYER_ID + 1);
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
    table().nextId = 1;
    setTransparencySortMode(TransparencySortMode::DEFAULT);
    setTransparencySortAxis(Vector3(0.0f, 1.0f, 0.0f));
}
